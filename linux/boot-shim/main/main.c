// SPDX-License-Identifier: Apache-2.0 OR GPL-2.0-only
/*
 * EasyStick Stamp-P4 M1 boot shim.
 *
 * The ESP-IDF app is deliberately small: validate a flat RV32 Image, copy
 * it and the project DTS into HEX PSRAM, map the read-only squashfs partition,
 * patch the DTB's mtd-rom placeholder, and jump to Linux in M-mode. C6,
 * SDIO, USB gadget, and network initialization do not belong in this shim.
 *
 * Flash writes are restricted to validated watchdog crash capsules and the
 * inactive boot-image A/B slot. Bad Linux artifacts still restart without
 * touching the boot map.
 */

#include <inttypes.h>
#include <stddef.h>
#include <stdbool.h>
#include <stdint.h>
#include <string.h>

#include "esp_attr.h"
#include "esp_cpu.h"
#include "esp_intr_alloc.h"
#include "esp_log.h"
#include "esp_partition.h"
#include "esp_rom_sys.h"
#include "esp_system.h"
#include "esp_private/hw_stack_guard.h"
#include "esp_private/interrupt_clic.h"
#include "esp32p4/rom/ets_sys.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/idf_additions.h"
#include "hal/cache_hal.h"
#include "hal/crosscore_int_ll.h"
#include "hal/cpu_utility_ll.h"
#include "hal/interrupt_clic_ll.h"
#include "riscv/interrupt.h"
#include "soc/clic_reg.h"
#include "soc/hp_system_reg.h"
#include "soc/soc.h"
#include "soc/systimer_reg.h"
#include "soc/system_intr.h"

#ifdef EASYSTICK_TSENS_ORACLE
#include "tsens_oracle.h"
#endif

#ifdef EASYSTICK_CMD53_RETENTION_BB
#include "easystick_cmd53_bb_boot.h"
#endif

#ifdef EASYSTICK_C68_CLEAN_RELEASE
#include "sdkconfig.h"
#if !CONFIG_ESP_SYSTEM_SINGLE_CORE_MODE
#error "C68 requires CONFIG_ESP_SYSTEM_SINGLE_CORE_MODE"
#endif
#endif

#ifdef EASYSTICK_M2_SDIO
#include "m2_sdmmc.h"
#endif

#include "hal/usb_wrap_ll.h"
#include "soc/usb_wrap_struct.h"
#include "esp_rom_gpio.h"
#include "soc/gpio_sig_map.h"
#include "soc/gpio_pins.h"
#include "driver/gpio.h"
#include "boot_ab.h"

static void easystick_usb_otg_init(void)
{
	ESP_LOGI("easystick-otg", "enabling USB OTG 1.1 (FS GPIO26/27) clock and PHY...");
	_usb_wrap_ll_enable_bus_clock(true);
	_usb_wrap_ll_reset_register();
	usb_wrap_ll_phy_set_defaults(&USB_WRAP);
	usb_wrap_ll_phy_select(&USB_WRAP, 1);

	/* Force USB OTG 1.1 into peripheral device mode with active VBUS session */
	esp_rom_gpio_connect_in_signal(GPIO_MATRIX_CONST_ONE_INPUT, USB_OTG11_IDDIG_PAD_IN_IDX, false);
	esp_rom_gpio_connect_in_signal(GPIO_MATRIX_CONST_ONE_INPUT, USB_SRP_BVALID_PAD_IN_IDX, false);
	esp_rom_gpio_connect_in_signal(GPIO_MATRIX_CONST_ONE_INPUT, USB_OTG11_VBUSVALID_PAD_IN_IDX, false);
	esp_rom_gpio_connect_in_signal(GPIO_MATRIX_CONST_ZERO_INPUT, USB_OTG11_AVALID_PAD_IN_IDX, false);

	/* Configure drive capability for USB D- (GPIO26) and D+ (GPIO27) */
	gpio_set_drive_capability(GPIO_NUM_26, GPIO_DRIVE_CAP_3);
	gpio_set_drive_capability(GPIO_NUM_27, GPIO_DRIVE_CAP_3);

	/* Keep D+ pullup OFF during boot so host does not enumerate prematurely */
	USB_WRAP.otg_conf.pad_pull_override = 1;
	USB_WRAP.otg_conf.dp_pullup = 0;
	gpio_pullup_dis(GPIO_NUM_27);
}

static const char *TAG = "easystick-boot";

/* Patched ESP-IDF vectors.S C13 hook; must stay linked (zero = inert). */
volatile uint32_t c13_redirect_pc IRAM_DATA_ATTR;
volatile uint32_t c13_frame_patched IRAM_DATA_ATTR;

#define KERNEL_LOAD_PA 0x48000000u
#define DTB_LOAD_PA 0x48800000u
#define BOOT_LOAD_PA 0x499c0000u
#define MMU_PAGE_BYTES 0x10000u
#define RISCV_IMAGE_MAGIC 0x5643534952ULL

_Static_assert(sizeof(struct boot_update_request) == 20,
	       "boot update request protocol changed");
_Static_assert(sizeof(struct boot_metadata) == 32,
	       "boot metadata layout changed");

static uint32_t ieee_crc32_update(uint32_t crc, const uint8_t *data,
				  uint32_t bytes)
{
	while (bytes--) {
		crc ^= *data++;
		for (unsigned int bit = 0; bit < 8; ++bit)
			crc = (crc >> 1) ^ ((crc & 1u) ? 0xedb88320u : 0u);
	}
	return crc;
}

static uint32_t ieee_crc32(const void *data, uint32_t bytes)
{
	return ieee_crc32_update(0xffffffffu, data, bytes) ^ 0xffffffffu;
}

static esp_err_t partition_crc32(const esp_partition_t *part, uint32_t bytes,
				 uint32_t *crc_out)
{
	uint8_t buf[1024];
	uint32_t offset = 0;
	uint32_t crc = 0xffffffffu;

	if (bytes > part->size)
		return ESP_ERR_INVALID_SIZE;
	while (offset < bytes) {
		uint32_t count = bytes - offset;
		if (count > sizeof(buf))
			count = sizeof(buf);
		esp_err_t err = esp_partition_read(part, offset, buf, count);
		if (err != ESP_OK)
			return err;
		crc = ieee_crc32_update(crc, buf, count);
		offset += count;
	}
	*crc_out = crc ^ 0xffffffffu;
	return ESP_OK;
}

static bool boot_metadata_valid(const esp_partition_t *meta,
				const esp_partition_t *slots[2],
				struct boot_metadata *record)
{
	uint32_t actual_crc;

	if (!meta || meta->size < sizeof(*record) ||
	    esp_partition_read(meta, 0, record, sizeof(*record)) != ESP_OK)
		return false;
	if (record->magic != BOOT_META_MAGIC ||
	    record->version != BOOT_META_VERSION ||
	    record->commit_word != BOOT_META_COMMIT || record->slot > 1 ||
	    record->image_bytes != BOOT_IMAGE_BYTES || !slots[record->slot] ||
	    slots[record->slot]->size < BOOT_IMAGE_BYTES)
		return false;
	if (record->record_crc32 != ieee_crc32(record,
					      offsetof(struct boot_metadata, record_crc32)))
		return false;
	if (partition_crc32(slots[record->slot], record->image_bytes,
			    &actual_crc) != ESP_OK)
		return false;
	return actual_crc == record->image_crc32;
}

static bool generation_is_newer(uint32_t candidate, uint32_t current)
{
	return (int32_t)(candidate - current) > 0;
}

static void clear_boot_update_request(void)
{
	volatile struct boot_update_request *request =
		(volatile struct boot_update_request *)(uintptr_t)BOOT_UPDATE_REQUEST_PA;

	/* Clearing the commit field first makes any torn clear safely invalid. */
	request->commit_word = 0;
	cache_hal_writeback_addr(BOOT_UPDATE_REQUEST_PA,
				 sizeof(*request));
	cache_hal_invalidate_addr(BOOT_UPDATE_REQUEST_PA,
				  sizeof(*request));
}

static bool boot_update_request_valid(struct boot_update_request *request)
{
	const volatile struct boot_update_request *src =
		(const volatile struct boot_update_request *)(uintptr_t)BOOT_UPDATE_REQUEST_PA;

	cache_hal_invalidate_addr(BOOT_UPDATE_REQUEST_PA, sizeof(*request));
	memcpy(request, (const void *)src, sizeof(*request));
	if (request->magic != BOOT_REQUEST_MAGIC ||
	    request->version != BOOT_REQUEST_VERSION ||
	    request->image_bytes != BOOT_IMAGE_BYTES ||
	    request->commit_word != BOOT_REQUEST_COMMIT)
		return false;
	return ieee_crc32((const void *)(uintptr_t)BOOT_LOAD_PA,
			  BOOT_IMAGE_BYTES) == request->image_crc32;
}

static bool write_boot_metadata(const esp_partition_t *meta,
				const struct boot_metadata *record)
{
	const uint32_t commit_offset = offsetof(struct boot_metadata, commit_word);
	struct boot_metadata readback;
	esp_err_t err;

	if (!meta || meta->size < 0x1000u)
		return false;
	err = esp_partition_erase_range(meta, 0, 0x1000u);
	if (err != ESP_OK)
		return false;
	err = esp_partition_write(meta, 0, record, commit_offset);
	if (err != ESP_OK)
		return false;
	/* The only transition which makes this sector bootable is last. */
	err = esp_partition_write(meta, commit_offset, &record->commit_word,
				  sizeof(record->commit_word));
	if (err != ESP_OK)
		return false;
	if (esp_partition_read(meta, 0, &readback, sizeof(readback)) != ESP_OK)
		return false;
	return memcmp(&readback, record, sizeof(readback)) == 0;
}

/* Select an already verified image, optionally committing a Linux-staged
 * update.  The inactive slot and inactive metadata sector are the only flash
 * erase targets, leaving a bootable image and metadata record on power loss. */
static const esp_partition_t *select_boot_partition(void)
{
	const esp_partition_t *slots[2] = {
		esp_partition_find_first(ESP_PARTITION_TYPE_DATA,
					 ESP_PARTITION_SUBTYPE_ANY, "boot"),
		esp_partition_find_first(ESP_PARTITION_TYPE_DATA,
					 ESP_PARTITION_SUBTYPE_ANY, "boot_alt"),
	};
	const esp_partition_t *metas[2] = {
		esp_partition_find_first(ESP_PARTITION_TYPE_DATA,
					 ESP_PARTITION_SUBTYPE_ANY, "bootmeta0"),
		esp_partition_find_first(ESP_PARTITION_TYPE_DATA,
					 ESP_PARTITION_SUBTYPE_ANY, "bootmeta1"),
	};
	struct boot_metadata records[2];
	bool valid[2];
	int selected_meta = -1;
	uint32_t selected_slot = 0;
	struct boot_update_request request;

	if (!slots[0] || slots[0]->size < BOOT_IMAGE_BYTES) {
		ESP_LOGW(TAG, "boot slot0 missing or too small");
		return NULL;
	}
	valid[0] = boot_metadata_valid(metas[0], slots, &records[0]);
	valid[1] = boot_metadata_valid(metas[1], slots, &records[1]);
	if (valid[0] && (!valid[1] ||
			 generation_is_newer(records[0].generation, records[1].generation)))
		selected_meta = 0;
	else if (valid[1])
		selected_meta = 1;
	if (selected_meta >= 0)
		selected_slot = records[selected_meta].slot;
	else
		ESP_LOGW(TAG, "boot metadata unavailable; falling back to slot0");

	if (boot_update_request_valid(&request)) {
		uint32_t active_crc = 0;
		bool same_image = partition_crc32(slots[selected_slot], BOOT_IMAGE_BYTES,
						 &active_crc) == ESP_OK &&
				  active_crc == request.image_crc32;
		if (same_image) {
			ESP_LOGI(TAG, "boot update already active; clearing request");
			clear_boot_update_request();
		} else if (slots[1u - selected_slot] &&
			   slots[1u - selected_slot]->size >= BOOT_IMAGE_BYTES) {
			uint32_t inactive_slot = 1u - selected_slot;
			uint32_t verify_crc = 0;
			struct boot_metadata next = {
				.magic = BOOT_META_MAGIC,
				.version = BOOT_META_VERSION,
				.generation = selected_meta >= 0 ?
					records[selected_meta].generation + 1u : 1u,
				.slot = inactive_slot,
				.image_bytes = BOOT_IMAGE_BYTES,
				.image_crc32 = request.image_crc32,
				.commit_word = BOOT_META_COMMIT,
			};
			next.record_crc32 = ieee_crc32(&next,
						      offsetof(struct boot_metadata, record_crc32));
			if (esp_partition_erase_range(slots[inactive_slot], 0,
						      BOOT_IMAGE_BYTES) == ESP_OK &&
			    esp_partition_write(slots[inactive_slot], 0,
						(const void *)(uintptr_t)BOOT_LOAD_PA,
						BOOT_IMAGE_BYTES) == ESP_OK &&
			    partition_crc32(slots[inactive_slot], BOOT_IMAGE_BYTES,
					    &verify_crc) == ESP_OK &&
			    verify_crc == request.image_crc32 &&
			    write_boot_metadata(metas[selected_meta == 0 ? 1 : 0], &next)) {
				selected_slot = inactive_slot;
				selected_meta = selected_meta == 0 ? 1 : 0;
				records[selected_meta] = next;
				ESP_LOGI(TAG, "boot update committed: generation=%" PRIu32
					 " slot=%" PRIu32 " crc=%08" PRIx32,
					 next.generation, next.slot, next.image_crc32);
			} else {
				ESP_LOGE(TAG, "boot update failed; retaining active slot=%" PRIu32,
					 selected_slot);
				return slots[selected_slot];
			}
			clear_boot_update_request();
		} else {
			ESP_LOGE(TAG, "boot update cannot proceed; inactive slot missing");
			return slots[selected_slot];
		}
	}

	if (selected_meta >= 0)
		ESP_LOGI(TAG, "boot selection: generation=%" PRIu32 " slot=%" PRIu32
			 " crc=%08" PRIx32, records[selected_meta].generation,
			 records[selected_meta].slot, records[selected_meta].image_crc32);
	else
		ESP_LOGW(TAG, "boot selection: legacy fallback slot0");
	return slots[selected_slot];
}

static void fatal_restart(const char *reason)
{
	ESP_LOGE(TAG, "fatal: %s; restarting without flash writes", reason);
	vTaskDelay(pdMS_TO_TICKS(100));
	esp_restart();
}

static esp_err_t load_partition(const esp_partition_t *part, uint32_t dst_pa,
					uint32_t bytes)
{
	uint8_t *dst = (uint8_t *)(uintptr_t)dst_pa;
	uint32_t done = 0;
	cache_hal_invalidate_addr(dst_pa, bytes);

	while (done < bytes) {
		uint32_t part_off = done & ~(MMU_PAGE_BYTES - 1u);
		uint32_t intra = done - part_off;
		uint32_t count = bytes - done;
		if (count > MMU_PAGE_BYTES - intra)
			count = MMU_PAGE_BYTES - intra;

		const void *mapped = NULL;
		esp_partition_mmap_handle_t handle = 0;
		esp_err_t err = esp_partition_mmap(part, part_off, MMU_PAGE_BYTES,
							 ESP_PARTITION_MMAP_DATA,
							 &mapped, &handle);
		if (err != ESP_OK)
			return err;
		memcpy(dst + done, (const uint8_t *)mapped + intra, count);
		esp_partition_munmap(handle);
		done += count;
	}

	/* The kernel is fetched as instructions immediately after this copy.
	 * Drop stale PSRAM lines before the memcpy, then write back and invalidate
	 * again so the instruction path cannot execute an old mapping. ESP-IDF's
	 * PSRAM loader follows the same invalidate/copy/invalidate discipline. */
	cache_hal_writeback_addr(dst_pa, bytes);
	cache_hal_invalidate_addr(dst_pa, bytes);
	return ESP_OK;
}

static uint32_t kernel_image_size(const esp_partition_t *part)
{
	const void *mapped = NULL;
	esp_partition_mmap_handle_t handle = 0;
	if (esp_partition_mmap(part, 0, MMU_PAGE_BYTES, ESP_PARTITION_MMAP_DATA,
				       &mapped, &handle) != ESP_OK)
		return 0;

	uint64_t magic = 0;
	uint64_t image_size = 0;
	memcpy(&magic, (const uint8_t *)mapped + 48, sizeof(magic));
	memcpy(&image_size, (const uint8_t *)mapped + 16, sizeof(image_size));
	esp_partition_munmap(handle);
	if (magic != RISCV_IMAGE_MAGIC || image_size == 0 || image_size > part->size)
		return 0;
	return (uint32_t)image_size;
}

/* Linux runs NOMMU userspace in U-mode.  Give it RWX access to the 64 MiB
 * PSRAM window while leaving the entry unlocked so the kernel can tighten it
 * later.  This is the same PMP boundary used by the reference P4 port. */
static void IRAM_ATTR grant_psram_to_umode(void)
{
	const unsigned long pmpaddr9 = 0x127fffffUL;
	const unsigned long cfg_byte = 0x1fUL; /* NAPOT + RWX, unlocked */
	unsigned long cfg2;
	__asm__ volatile("csrw pmpaddr9, %0" :: "r"(pmpaddr9));
	__asm__ volatile("csrr %0, pmpcfg2" : "=r"(cfg2));
	cfg2 = (cfg2 & ~(0xffUL << 8)) | (cfg_byte << 8);
	__asm__ volatile("csrw pmpcfg2, %0" :: "r"(cfg2));
}

static bool patch_rootfs_window(uint8_t *dtb, uint32_t total,
					uint32_t rootfs_va, uint32_t rootfs_bytes)
{
	static const uint8_t needle[8] = {
		0xde, 0xad, 0xbe, 0xef, 0xca, 0xfe, 0xfa, 0xde
	};
	uint8_t replacement[8] = {
		(uint8_t)(rootfs_va >> 24), (uint8_t)(rootfs_va >> 16),
		(uint8_t)(rootfs_va >> 8), (uint8_t)rootfs_va,
		(uint8_t)(rootfs_bytes >> 24), (uint8_t)(rootfs_bytes >> 16),
		(uint8_t)(rootfs_bytes >> 8), (uint8_t)rootfs_bytes,
	};

	for (uint32_t i = 0; i + sizeof(needle) <= total; ++i) {
		if (!memcmp(dtb + i, needle, sizeof(needle))) {
			memcpy(dtb + i, replacement, sizeof(replacement));
			return true;
		}
	}
	return false;
}

static void IRAM_ATTR jump_to_linux(uint32_t entry, uint32_t dtb_pa)
{
	__asm__ volatile("csrci mstatus, 0x8\n"
			     "fence.i\n"
			     "mv a0, zero\n"
			     "mv a1, %0\n"
			     "jr %1\n"
			     :
			     : "r"(dtb_pa), "r"(entry)
			     : "a0", "a1");
	__builtin_unreachable();
}

#ifdef EASYSTICK_C68_CLEAN_RELEASE

#define C68_STAGE_WAITING      0xC6810001u
#define C68_RELEASE_GO         0xC68A11A5u
#define C68_CPU1_WAIT_CYCLES   72000000u

volatile uint32_t c68_release IRAM_DATA_ATTR;
volatile uint32_t c68_stage IRAM_DATA_ATTR;

extern void c68_secondary_entry(void);

static void prepare_cpu1_c68_clean_release(void)
{
	uint32_t deadline;

	c68_release = 0;
	c68_stage = 0;
	__asm__ volatile("fence rw, rw" ::: "memory");

	ets_set_appcpu_boot_addr(0);
	esp_cpu_unstall(1);
	cpu_utility_ll_enable_clock_and_reset_app_cpu();
	ets_set_appcpu_boot_addr((uint32_t)(uintptr_t)c68_secondary_entry);

	deadline = esp_cpu_get_cycle_count() + C68_CPU1_WAIT_CYCLES;
	while (__atomic_load_n(&c68_stage, __ATOMIC_RELAXED) != C68_STAGE_WAITING) {
		if (esp_cpu_get_cycle_count() > deadline)
			fatal_restart("C68 CPU1 never reached WAITING");
	}
	ets_printf("easystick-boot: C68 WAIT stage=1\n");
	ESP_LOGI(TAG, "C68 CPU1 WAITING; CPU0 -> Linux");
}

#else /* !EASYSTICK_C68_CLEAN_RELEASE */

#ifndef EASYSTICK_UP_BOOT

/*
 * CPU1 bring-up (probe C66): MID_1107475_ALARM.
 * C64 delay (+1,107,475 + C36 720k→stall) + SYSTIMER/CLIC pending snapshot.
 * Same 2 PA reads of s_handled_systicks[1] for timing footprint; UART A=%03X.
 * RTS×200 valid; primary metric is ALARM12 × outcome.
 *
 * Forbidden: INIT/stack fill, ready-list ops, FROM_CPU_1, CLIC/CSR writes,
 * IPI, MEPC/trampoline, esp_ipc, HPCORE1_SW_RESET, FreeRTOS task park,
 * non-returning ISR.
 */

volatile uint32_t c13_isr_seen IRAM_DATA_ATTR;
volatile uint32_t c13_tramp_entered IRAM_DATA_ATTR;

#define EASYSTICK_CPU1_HANDOFF_PA 0x487FF000u
#define C33_STACK_DEPTH 2048u
#define C40_EXPECT_RELEASE_PA 0x4ff15f1cu
#define C40_EXPECT_TCB 0x4ff128c4u
#define C40_EXPECT_STACK 0x4ff120c4u
#define C40_EXPECT_INIT 0x4ff0b210u
#define C40_EXPECT_VAL 0x4ff0b330u
#define C40_EXPECT_FN 0x4000ba3cu

/* Length-matched to C39 SNAP1 format; linked-but-not-executed on C41. */
#define C40_UART_FMT "easystick-boot: QQ0 WWWWW tasks=%u ready1=%u top=%u\n"
_Static_assert(sizeof(C40_UART_FMT) == 53,
	       "C40 UART format length must match C39 (52 chars + NUL)");
/* C66: C64 delay; alarm/pending snapshot (post-stall read only). */
#define C66_MID_CYCLES 1107475u
#define C66_CLIC_SLOT_SYSTIMER0 17u
#define C66_CLIC_SLOT_SYSTIMER1 18u
/*
 * Absolute PA of FreeRTOS s_handled_systicks[] in the KEEP layout (C58-era).
 * [1] is at base+4. Reading via PA avoids freertos .text that shifts DRAM KEEP.
 */
#define C59_HANDLED_SYSTICKS_PA 0x4ff15e90u /* patched from nm after pass1 */

static uint32_t c59_get_core1_systick_epoch(void)
{
	volatile uint32_t *base =
		(volatile uint32_t *)(uintptr_t)C59_HANDLED_SYSTICKS_PA;
	if (C59_HANDLED_SYSTICKS_PA == 0u)
		return 0;
	return __atomic_load_n(&base[1], __ATOMIC_RELAXED);
}

static uint32_t c66_clic_ctrl_word(uint32_t hart, uint32_t slot)
{
	uintptr_t addr = DR_REG_CLIC_CTRL_BASE + hart * DUALCORE_CLIC_CTRL_OFF +
			 (uintptr_t)slot * 4u;

	return REG_READ(addr);
}

static uint32_t c66_alarm_pending12(void)
{
	uint32_t raw = REG_READ(SYSTIMER_INT_RAW_REG) & 7u;
	uint32_t st = REG_READ(SYSTIMER_INT_ST_REG) & 7u;
	uint32_t clic17 = c66_clic_ctrl_word(1, C66_CLIC_SLOT_SYSTIMER0);
	uint32_t clic18 = c66_clic_ctrl_word(1, C66_CLIC_SLOT_SYSTIMER1);
	uint32_t clic17_h0 = c66_clic_ctrl_word(0, C66_CLIC_SLOT_SYSTIMER0);
	uint32_t mip;
	uint32_t pack;

	__asm__ volatile("csrr %0, mip" : "=r"(mip));
	pack = raw | (st << 3);
	if (clic17 & CLIC_INT_IP)
		pack |= BIT(6);
	if (clic17 & CLIC_INT_IE)
		pack |= BIT(7);
	if (clic18 & CLIC_INT_IP)
		pack |= BIT(8);
	if (clic18 & CLIC_INT_IE)
		pack |= BIT(9);
	if (clic17_h0 & CLIC_INT_IP)
		pack |= BIT(10);
	if (mip & 0x80u)
		pack |= BIT(11);
	return pack & 0xfffu;
}

static volatile uint32_t s_c33_iram_release IRAM_DATA_ATTR;
static volatile uint32_t s_c33_ran;
static DRAM_ATTR StaticTask_t s_c33_tcb;
static DRAM_ATTR StackType_t s_c33_stack[C33_STACK_DEPTH];

static void cpu0_c33_should_not_run(void *arg)
{
	(void)arg;
	s_c33_ran = 1;
	for (;;) {
		__asm__ volatile("nop");
	}
}

/* Linked-but-not-executed: preserves C40 print call without BSS growth. */
__attribute__((noinline, used)) static void c41_dead_c40_print(void)
{
	ets_printf(C40_UART_FMT, (unsigned)5, (unsigned)0, (unsigned)2);
}

static void c40_keep_c35_layout_symbols(void)
{
	volatile uintptr_t sink;

	sink = (uintptr_t)&s_c33_tcb;
	sink ^= (uintptr_t)s_c33_stack;
	sink ^= (uintptr_t)cpu0_c33_should_not_run;
	sink ^= (uintptr_t)easystick_c33_init_only;
	sink ^= (uintptr_t)easystick_c33_validate_tcb;
	sink ^= (uintptr_t)easystick_c33_sched_snapshot;
	sink ^= (uintptr_t)&s_c33_ran;
	sink ^= (uintptr_t)C40_UART_FMT;
	sink ^= (uintptr_t)c41_dead_c40_print;
	(void)sink;
}

static void prepare_cpu1_linux_trampoline(uint32_t entry, uint32_t dtb_pa)
{
	UBaseType_t pre;
	UBaseType_t post;
	unsigned bump_applied;
	uint32_t t0;
	uint32_t release_pa;
	volatile uint32_t *handoff = (volatile uint32_t *)(uintptr_t)EASYSTICK_CPU1_HANDOFF_PA;

	(void)entry;
	(void)dtb_pa;
	c13_redirect_pc = 0;
	c13_frame_patched = 0;
	c13_isr_seen = 0;
	c13_tramp_entered = 0;

	c40_keep_c35_layout_symbols();

	if ((uint32_t)(uintptr_t)&s_c33_tcb != C40_EXPECT_TCB ||
	    (uint32_t)(uintptr_t)s_c33_stack != C40_EXPECT_STACK ||
	    (uint32_t)(uintptr_t)easystick_c33_init_only != C40_EXPECT_INIT ||
	    (uint32_t)(uintptr_t)easystick_c33_validate_tcb != C40_EXPECT_VAL ||
	    (uint32_t)(uintptr_t)cpu0_c33_should_not_run != C40_EXPECT_FN)
		fatal_restart("C40 layout INVALID symbol addresses vs C35");

	s_c33_iram_release = 0;
	release_pa = (uint32_t)(uintptr_t)&s_c33_iram_release;
	handoff[0] = release_pa;
	__asm__ volatile("fence rw, rw" ::: "memory");
	if (handoff[0] != release_pa)
		fatal_restart("C40 HANDOFF PA mismatch");
	if (release_pa != C40_EXPECT_RELEASE_PA)
		fatal_restart("C40 layout INVALID release_pa vs C35");

	ESP_LOGI(TAG, "CPU1 prep: C36 layout-keep C29-runtime + stall");
	ets_printf("easystick-boot: C36 HANDOFF release_pa=0x%08x\n",
		   (unsigned)handoff[0]);

	pre = uxTaskPriorityGet(NULL);
	bump_applied = 0;
	if (pre < 2) {
		vTaskPrioritySet(NULL, 2);
		bump_applied = 1;
	}
	post = uxTaskPriorityGet(NULL);
	ets_printf("easystick-boot: C36 PRE_PRIO=%u POST_PRIO=%u BUMP_APPLIED=%u\n",
		   (unsigned)pre, (unsigned)post, bump_applied);
	if (pre != 1 || post != 2 || bump_applied != 1)
		fatal_restart("C40 INVALID priority mutation vs C29");

	/* C66: C64 delay + alarm/pending snapshot; same 2 epoch reads as C64. */
	{
		uint32_t tick_before;
		uint32_t tick_after;
		uint32_t alarm12;

		t0 = esp_cpu_get_cycle_count();
		tick_before = c59_get_core1_systick_epoch();
		while ((esp_cpu_get_cycle_count() - t0) < C66_MID_CYCLES) {
			__asm__ volatile("nop");
		}
		(void)tick_before;

		s_c33_ran = 0;
		t0 = esp_cpu_get_cycle_count();
		while ((esp_cpu_get_cycle_count() - t0) < 720000u) {
			__asm__ volatile("nop");
		}
		if (s_c33_ran != 0)
			fatal_restart("C40 RAN flipped during busy-wait");

		ets_printf("easystick-boot: C36 CPU0_STALL_BEFORE\n");
		esp_cpu_stall(1);
		/* CPU1 stalled: read SYSTIMER/CLIC snapshot only after stall. */
		tick_after = c59_get_core1_systick_epoch();
		(void)tick_after;
		alarm12 = c66_alarm_pending12();
		ets_printf("easystick-boot: C36 CPU0_STALL_AFTER; A=%03X; CPU0 -> Linux\n",
			   (unsigned)alarm12);
		if (s_c33_ran != 0)
			fatal_restart("C40 RAN flipped at stall");
	}
	ESP_LOGI(TAG, "C36 C29-runtime+CPU1 stalled; jump Linux");
}

#endif /* !EASYSTICK_UP_BOOT */
#endif /* EASYSTICK_C68_CLEAN_RELEASE */

void app_main(void)
{
#ifdef EASYSTICK_CMD53_RETENTION_BB
	/*
	 * Retention dump must run before C6/SDIO/Linux handoff. Do not clear
	 * the black-box until the dump and optional self-test have completed.
	 */
	easystick_cmd53_bb_dump_early();
	easystick_crash_capsule_dump_early();
	easystick_crash_capsule_flush();
#ifdef EASYSTICK_CMD53_BB_SELFTEST_TORN
	easystick_cmd53_bb_selftest_torn_maybe();
#elif defined(EASYSTICK_CMD53_BB_SELFTEST)
	easystick_cmd53_bb_selftest_maybe();
#endif
	easystick_cmd53_bb_release_after_dump();
#endif
#ifdef EASYSTICK_TSENS_ORACLE
	if (!easystick_tsens_oracle_run())
		fatal_restart("TSENS oracle failed");
#endif
	#ifdef EASYSTICK_M2_SDIO
	/* M2 opts in to the raw Slot 1 bootstrap; M1 keeps the proven path. */
	easystick_m2_sdmmc_init();
	#endif
	/* Enable Full-Speed USB OTG (DWC2) for Type-A USB Gadget */
	easystick_usb_otg_init();
	const esp_partition_t *kernel = esp_partition_find_first(
		ESP_PARTITION_TYPE_DATA, ESP_PARTITION_SUBTYPE_ANY, "kernel");
	const esp_partition_t *dtb_part = esp_partition_find_first(
		ESP_PARTITION_TYPE_DATA, ESP_PARTITION_SUBTYPE_ANY, "dtb");
	const esp_partition_t *rootfs = esp_partition_find_first(
		ESP_PARTITION_TYPE_DATA, ESP_PARTITION_SUBTYPE_ANY, "rootfs");
	if (!kernel || !dtb_part || !rootfs)
		fatal_restart("kernel/dtb/rootfs partition missing");

	uint32_t image_bytes = kernel_image_size(kernel);
	if (!image_bytes)
		fatal_restart("kernel is not a valid flat RISC-V Image");
	if (load_partition(kernel, KERNEL_LOAD_PA, image_bytes) != ESP_OK)
		fatal_restart("kernel copy failed");
	if (load_partition(dtb_part, DTB_LOAD_PA, dtb_part->size) != ESP_OK)
		fatal_restart("DTB copy failed");

	const esp_partition_t *boot_part = select_boot_partition();
	if (boot_part) {
		if (load_partition(boot_part, BOOT_LOAD_PA, BOOT_IMAGE_BYTES) == ESP_OK) {
			/* The selected, CRC-validated flash slot is authoritative. */
			ESP_LOGI(TAG, "boot slot loaded from flash to PSRAM 0x%08x (%u bytes; flash is authoritative)",
				 BOOT_LOAD_PA, BOOT_IMAGE_BYTES);
		} else {
			ESP_LOGW(TAG, "boot slot copy to PSRAM failed; continuing");
		}
	} else {
		ESP_LOGW(TAG, "no valid boot slot available");
	}

	const void *rootfs_va = NULL;
	esp_partition_mmap_handle_t rootfs_handle = 0;
	if (esp_partition_mmap(rootfs, 0, rootfs->size, ESP_PARTITION_MMAP_DATA,
				       &rootfs_va, &rootfs_handle) != ESP_OK)
		fatal_restart("rootfs mapping failed");
	const uint8_t *root_magic = rootfs_va;
	if (root_magic[0] != 'h' || root_magic[1] != 's' ||
	    root_magic[2] != 'q' || root_magic[3] != 's')
		fatal_restart("rootfs is not squashfs");

	uint8_t *dtb = (uint8_t *)(uintptr_t)DTB_LOAD_PA;
	if (dtb[0] != 0xd0 || dtb[1] != 0x0d || dtb[2] != 0xfe || dtb[3] != 0xed)
		fatal_restart("DTB magic missing");
	uint32_t total = ((uint32_t)dtb[4] << 24) | ((uint32_t)dtb[5] << 16) |
				 ((uint32_t)dtb[6] << 8) | dtb[7];
	if (total < 16 || total > dtb_part->size)
		fatal_restart("DTB totalsize outside partition");
	if (!patch_rootfs_window(dtb, total, (uint32_t)(uintptr_t)rootfs_va,
					 rootfs->size))
		fatal_restart("mtd-rom placeholder missing from DTB");
	cache_hal_writeback_addr(DTB_LOAD_PA, total);

	ESP_LOGI(TAG, "Linux artifacts valid; kernel=%" PRIu32 " bytes, rootfs=%" PRIu32 " bytes", image_bytes, rootfs->size);
#ifdef EASYSTICK_C68_CLEAN_RELEASE
	ESP_LOGI(TAG, "C68 clean-release: CPU1 bare-metal WAITING, CPU0 -> Linux 0x%08x DTB 0x%08x",
		 KERNEL_LOAD_PA, DTB_LOAD_PA);
#else
	ESP_LOGI(TAG, "prepare CPU1 park trampoline, CPU0 -> Linux 0x%08x DTB 0x%08x",
		 KERNEL_LOAD_PA, DTB_LOAD_PA);
#endif
	vTaskDelay(pdMS_TO_TICKS(20));
	/* rootfs_handle intentionally remains mapped across the Linux jump. */
	(void)rootfs_handle;
	esp_hw_stack_guard_monitor_stop();
	grant_psram_to_umode();
#ifdef EASYSTICK_C68_CLEAN_RELEASE
	prepare_cpu1_c68_clean_release();
#elif defined(EASYSTICK_UP_BOOT)
	ESP_LOGI(TAG, "UP boot: CONFIG_SMP=n; CPU1 preparation skipped");
#else
	/*
	 * Probe C29: C27 priority-bump on C28 fixture (rate ×20).
	 */
	prepare_cpu1_linux_trampoline(KERNEL_LOAD_PA, DTB_LOAD_PA);
#endif
	jump_to_linux(KERNEL_LOAD_PA, DTB_LOAD_PA);
	fatal_restart("unexpected return from Linux");
}
