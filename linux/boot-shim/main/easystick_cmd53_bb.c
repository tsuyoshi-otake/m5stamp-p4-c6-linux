// SPDX-License-Identifier: Apache-2.0 OR GPL-2.0-only
/*
 * EasyStick CMD53 retention black-box — boot-shim side.
 */

#include <stdint.h>
#include <string.h>

#include "esp_attr.h"
#include "esp_partition.h"
#include "esp_rom_sys.h"
#include "esp_system.h"
#include "esp32p4/rom/ets_sys.h"

#include "easystick_cmd53_bb.h"

RTC_NOINIT_ATTR volatile struct easystick_rtc_c_window easystick_rtc_c;

static void easystick_beat_dump_early(void);
static void easystick_beat_init_unused(void);

static int crash_capsule_valid(
	volatile const struct easystick_crash_capsule *capsule)
{
	if (capsule->magic != EASYSTICK_CRASH_CAPSULE_MAGIC ||
	    capsule->version != EASYSTICK_CRASH_CAPSULE_VERSION ||
	    capsule->sequence == 0u ||
	    capsule->commit !=
		    (capsule->sequence ^ EASYSTICK_CRASH_CAPSULE_COMMIT_XOR) ||
	    (capsule->reason != EASYSTICK_CRASH_CAPSULE_REASON_WDT &&
	     capsule->reason != EASYSTICK_CRASH_CAPSULE_REASON_OOPS &&
	     capsule->reason != EASYSTICK_CRASH_CAPSULE_REASON_PANIC) ||
	    capsule->stack_len > EASYSTICK_CRASH_CAPSULE_STACK_BYTES)
		return 0;
	return 1;
}

static void crash_capsule_clear(volatile struct easystick_crash_capsule *capsule)
{
	volatile uint8_t *bytes = (volatile uint8_t *)capsule;

	for (size_t i = 0; i < sizeof(*capsule); ++i)
		bytes[i] = 0;
	__asm__ volatile("fence w, w" ::: "memory");
}

void easystick_crash_capsule_dump_early(void)
{
	volatile struct easystick_crash_capsule *capsule =
		&easystick_rtc_c.bb.crash;

	if (!crash_capsule_valid(capsule)) {
		ets_printf("easystick-boot: CRASH_CAPSULE empty/invalid\n");
		easystick_beat_dump_early();
		return;
	}

	ets_printf("easystick-boot: CRASH_CAPSULE VALID seq=%u reason=%u "
		   "captured_cpu=%u epc=0x%08x ra=0x%08x sp=0x%08x gp=0x%08x tp=0x%08x "
		   "status=0x%08x cause=0x%08x badaddr=0x%08x "
		   "wdt0=0x%08x wdt1=0x%08x wdt2=0x%08x int=0x%08x "
		   "stack_len=%u\n",
		   (unsigned)capsule->sequence, (unsigned)capsule->reason,
		   (unsigned)capsule->cpu, (unsigned)capsule->epc,
		   (unsigned)capsule->ra, (unsigned)capsule->sp,
		   (unsigned)capsule->gp, (unsigned)capsule->tp,
		   (unsigned)capsule->status, (unsigned)capsule->cause,
		   (unsigned)capsule->badaddr, (unsigned)capsule->wdt_config0,
		   (unsigned)capsule->wdt_config1, (unsigned)capsule->wdt_config2,
		   (unsigned)capsule->wdt_int_raw, (unsigned)capsule->stack_len);
	easystick_beat_dump_early();
}

static void easystick_beat_dump_early(void)
{
	unsigned i;

	for (i = 0; i < EASYSTICK_BEAT_CPUS; i++) {
		if (easystick_rtc_c.beat[i].cpu == EASYSTICK_BEAT_UNUSED_CPU) {
			ets_printf("easystick-boot: BEAT cpu=UNUSED slot=%u "
				   "seq=%u beat_jiffies=%u last_feed_jiffies=%u\n",
				   i,
				   (unsigned)easystick_rtc_c.beat[i].seq,
				   (unsigned)easystick_rtc_c.beat[i].beat_jiffies,
				   (unsigned)easystick_rtc_c.beat[i].last_feed_jiffies);
			continue;
		}
		ets_printf("easystick-boot: BEAT cpu=%u seq=%u beat_jiffies=%u "
			   "last_feed_jiffies=%u\n",
			   (unsigned)easystick_rtc_c.beat[i].cpu,
			   (unsigned)easystick_rtc_c.beat[i].seq,
			   (unsigned)easystick_rtc_c.beat[i].beat_jiffies,
			   (unsigned)easystick_rtc_c.beat[i].last_feed_jiffies);
	}
}

void easystick_crash_capsule_flush(void)
{
	const esp_partition_t *partition;
	volatile struct easystick_crash_capsule *capsule =
		&easystick_rtc_c.bb.crash;
	struct easystick_crash_capsule verify;
	uint32_t slot_count;
	uint32_t slot;
	esp_err_t err;

	if (!crash_capsule_valid(capsule))
		return;

	partition = esp_partition_find_first(
		ESP_PARTITION_TYPE_DATA, ESP_PARTITION_SUBTYPE_ANY, "crashlog");
	if (!partition || partition->size < 0x1000u ||
	    (partition->size % 0x1000u) != 0u) {
		ets_printf("easystick-boot: CRASH_CAPSULE FLASH_UNAVAILABLE\n");
		return;
	}

	slot_count = partition->size / 0x1000u;
	slot = capsule->sequence % slot_count;
	err = esp_partition_erase_range(partition, slot * 0x1000u, 0x1000u);
	if (err == ESP_OK)
		err = esp_partition_write(partition, slot * 0x1000u,
					  (const void *)capsule,
					  sizeof(*capsule));
	if (err == ESP_OK)
		err = esp_partition_read(partition, slot * 0x1000u, &verify,
					 sizeof(verify));
	if (err != ESP_OK) {
		ets_printf("easystick-boot: CRASH_CAPSULE FLASH_FAIL err=0x%x\n",
			   (unsigned)err);
		return;
	}
	if (memcmp(&verify, (const void *)capsule, sizeof(verify)) != 0) {
		ets_printf("easystick-boot: CRASH_CAPSULE FLASH_VERIFY_FAIL\n");
		return;
	}

	ets_printf("easystick-boot: CRASH_CAPSULE FLASH_OK slot=%u\n",
		   (unsigned)slot);
	crash_capsule_clear(capsule);
}

static unsigned stage_bit(volatile struct easystick_cmd53_bb *bb, uint32_t stage)
{
	return easystick_cmd53_bb_stage_active(stage, bb->generation) ? 1u : 0u;
}

void easystick_cmd53_bb_dump_early(void)
{
	volatile struct easystick_cmd53_bb *bb = &easystick_rtc_c.bb;
	uint32_t reason = (uint32_t)esp_reset_reason();
	uintptr_t pa = (uintptr_t)bb;
	uint32_t gen;

	ets_printf("easystick-boot: CMD53_BB pa=0x%08x reset_reason=%u\n",
		   (unsigned)pa, (unsigned)reason);
	ets_printf("easystick-boot: CMD52_MARKER magic=0x%08x armed=0x%08x "
		   "token_enter=0x%08x after_46=0x%08x before_47=0x%08x "
		   "after_47=0x%08x\n",
		   (unsigned)easystick_rtc_c.marker.magic,
		   (unsigned)easystick_rtc_c.marker.armed,
		   (unsigned)easystick_rtc_c.marker.token_enter,
		   (unsigned)easystick_rtc_c.marker.after_46,
		   (unsigned)easystick_rtc_c.marker.before_47,
		   (unsigned)easystick_rtc_c.marker.after_47);

	if (!easystick_cmd53_bb_valid(bb)) {
		ets_printf("easystick-boot: CMD53_BB empty/invalid magic=0x%08x gen=%u seq=%u commit=0x%08x\n",
			   (unsigned)bb->magic, (unsigned)bb->generation,
			   (unsigned)bb->event_seq, (unsigned)bb->commit);
		return;
	}

	gen = bb->generation;
	bb->reset_reason_hint = reason;
	ets_printf("easystick-boot: CMD53_BB VALID gen=%u seq=%u cmd_arg=0x%08x "
		   "req=%u cmd_done=%u cmd_err=%u idmac=%u data_over=%u data_err=%u "
		   "cto=%u dto=%u end=%u bb2=%u bb3=%u bb4=%u bb5=%u bb6=%u "
		   "bb4_ret=%d bb5_ret=%d bb6_ret=%d\n",
		   (unsigned)gen, (unsigned)bb->event_seq,
		   (unsigned)bb->cmd_arg,
		   stage_bit(bb, bb->stage_request),
		   stage_bit(bb, bb->stage_cmd_done),
		   stage_bit(bb, bb->stage_cmd_err),
		   stage_bit(bb, bb->stage_idmac),
		   stage_bit(bb, bb->stage_data_over),
		   stage_bit(bb, bb->stage_data_err),
		   stage_bit(bb, bb->stage_cto),
		   stage_bit(bb, bb->stage_dto),
		   stage_bit(bb, bb->stage_end),
		   stage_bit(bb, bb->stage_bb2),
		   stage_bit(bb, bb->stage_bb3),
		   stage_bit(bb, bb->stage_bb4),
		   stage_bit(bb, bb->stage_bb5),
		   stage_bit(bb, bb->stage_bb6),
		   (int)bb->bb4_ret, (int)bb->bb5_ret, (int)bb->bb6_ret);
	ets_printf("easystick-boot: CMD53_BB boundaries dma_in=%u dma_out=%u "
		   "bh_in=%u data_in=%u data_out=%u irq_out=%u\n",
		   stage_bit(bb, bb->stage_idmac_complete_enter),
		   stage_bit(bb, bb->stage_idmac_complete_exit),
		   stage_bit(bb, bb->stage_bh_enter),
		   stage_bit(bb, bb->stage_data_complete_enter),
		   stage_bit(bb, bb->stage_data_complete_exit),
		   stage_bit(bb, bb->stage_irq_exit));
	ets_printf("easystick-boot: CMD53_BB request_end enter=%u before_next=%u "
		   "after_next=%u idle=%u\n",
		   stage_bit(bb, bb->stage_request_end_enter),
		   stage_bit(bb, bb->stage_request_end_before_next),
		   stage_bit(bb, bb->stage_request_end_after_next),
		   stage_bit(bb, bb->stage_request_end_idle));

	/* Payload words are only meaningful when their stage tag matches gen. */
	if (easystick_cmd53_bb_stage_active(bb->stage_cmd_err, gen))
		ets_printf("easystick-boot: CMD53_BB cmd_st=0x%08x pending_cmd=0x%08x\n",
			   (unsigned)bb->cmd_status, (unsigned)bb->pending_cmd);
	else
		ets_printf("easystick-boot: CMD53_BB cmd_st=NA pending_cmd=NA\n");

	if (easystick_cmd53_bb_stage_active(bb->stage_data_err, gen) ||
	    easystick_cmd53_bb_stage_active(bb->stage_data_over, gen))
		ets_printf("easystick-boot: CMD53_BB data_st=0x%08x pending_data=0x%08x\n",
			   (unsigned)bb->data_status, (unsigned)bb->pending_data);
	else
		ets_printf("easystick-boot: CMD53_BB data_st=NA pending_data=NA\n");

	if (easystick_cmd53_bb_stage_active(bb->stage_idmac, gen))
		ets_printf("easystick-boot: CMD53_BB idsts=0x%08x\n",
			   (unsigned)bb->idsts);
	else
		ets_printf("easystick-boot: CMD53_BB idsts=NA\n");

	if (easystick_cmd53_bb_stage_active(bb->stage_bb3, gen))
		ets_printf("easystick-boot: CMD53_BB bb3_state=0x%08x bb3_pending=0x%08x\n",
			   (unsigned)bb->bb3_state,
			   (unsigned)bb->bb3_pending);
	else
		ets_printf("easystick-boot: CMD53_BB bb3_state=NA bb3_pending=NA\n");

	if (easystick_cmd53_bb_stage_active(bb->stage_end, gen))
		ets_printf("easystick-boot: CMD53_BB cmd_e=%d data_e=%d xfer=%u\n",
			   (int)bb->cmd_error, (int)bb->data_error,
			   (unsigned)bb->bytes_xfered);
	else
		ets_printf("easystick-boot: CMD53_BB cmd_e=NA data_e=NA xfer=NA\n");
}

static void easystick_beat_init_unused(void)
{
	unsigned i;

	for (i = 0; i < EASYSTICK_BEAT_CPUS; i++) {
		easystick_rtc_c.beat[i].seq = 0;
		easystick_rtc_c.beat[i].cpu = EASYSTICK_BEAT_UNUSED_CPU;
		easystick_rtc_c.beat[i].beat_jiffies = 0;
		easystick_rtc_c.beat[i].last_feed_jiffies = 0;
	}
}

static void bb_clear_identity(volatile struct easystick_cmd53_bb *bb)
{
	bb->commit = 0;
	bb->arm_state = EASYSTICK_CMD53_BB_ARM_FREE;
	bb->magic = 0;
	easystick_rtc_c.marker.magic = EASYSTICK_CMD52_MARKER_EMPTY;
	easystick_rtc_c.marker.armed = EASYSTICK_CMD52_MARKER_EMPTY;
	easystick_rtc_c.marker.token_enter = EASYSTICK_CMD52_MARKER_EMPTY;
	easystick_rtc_c.marker.after_46 = EASYSTICK_CMD52_MARKER_EMPTY;
	easystick_rtc_c.marker.before_47 = EASYSTICK_CMD52_MARKER_EMPTY;
	easystick_rtc_c.marker.after_47 = EASYSTICK_CMD52_MARKER_EMPTY;
	easystick_beat_init_unused();
	__asm__ volatile("fence w, w" ::: "memory");
}

void easystick_cmd53_bb_release_after_dump(void)
{
	/*
	 * CMD53_BB and the optional CMD52 marker are UART-dumped retention
	 * records, not a second persistent log. Release them after optional
	 * self-tests so the next Linux boot can arm one fresh target transfer.
	 * The crash capsule has its own flash flush and is intentionally
	 * unaffected.
	 */
	bb_clear_identity(&easystick_rtc_c.bb);
}

#if defined(EASYSTICK_CMD53_BB_SELFTEST) || defined(EASYSTICK_CMD53_BB_SELFTEST_TORN)
#ifdef EASYSTICK_CMD53_BB_SELFTEST_TORN
/*
 * Negative control: leave a torn update (commit invalidated, payload written,
 * final commit never sealed). After software restart must dump empty/invalid.
 */
void easystick_cmd53_bb_selftest_torn_maybe(void)
{
	volatile struct easystick_cmd53_bb *bb = &easystick_rtc_c.bb;

	if (easystick_cmd53_bb_valid(bb)) {
		ets_printf("easystick-boot: CMD53_BB TORN SELFTEST FAIL unexpected VALID\n");
		bb_clear_identity(bb);
		return;
	}

	if (bb->magic == EASYSTICK_CMD53_BB_MAGIC &&
	    bb->generation == EASYSTICK_CMD53_BB_TORN_GEN &&
	    bb->commit == 0 &&
	    bb->stage_request == EASYSTICK_CMD53_BB_TORN_GEN) {
		ets_printf("easystick-boot: CMD53_BB TORN SELFTEST PASS (invalid as required) reset_reason=%u\n",
			   (unsigned)esp_reset_reason());
		bb_clear_identity(bb);
		return;
	}

	/* Deliberately torn: invalidate + payload, no final commit. */
	bb->commit = 0;
	__asm__ volatile("fence w, w" ::: "memory");
	bb->magic = EASYSTICK_CMD53_BB_MAGIC;
	bb->version = EASYSTICK_CMD53_BB_VERSION;
	bb->generation = EASYSTICK_CMD53_BB_TORN_GEN;
	bb->event_seq = 1;
	bb->cmd_arg = 0x70E45E1Fu;
	bb->stage_request = EASYSTICK_CMD53_BB_TORN_GEN;
	__asm__ volatile("fence w, w" ::: "memory");
	/* Intentionally leave commit == 0. */
	ets_printf("easystick-boot: CMD53_BB TORN SELFTEST write (uncommitted); software restart\n");
	esp_restart();
}
#endif

#ifdef EASYSTICK_CMD53_BB_SELFTEST
/*
 * Positive control: sealed record survives software restart (not Chip Reset).
 */
void easystick_cmd53_bb_selftest_maybe(void)
{
	volatile struct easystick_cmd53_bb *bb = &easystick_rtc_c.bb;

	if (easystick_cmd53_bb_valid(bb) &&
	    bb->generation == EASYSTICK_CMD53_BB_SELFTEST_GEN &&
	    bb->stage_request == EASYSTICK_CMD53_BB_SELFTEST_GEN) {
		ets_printf("easystick-boot: CMD53_BB SELFTEST PASS reset_reason=%u\n",
			   (unsigned)esp_reset_reason());
		bb_clear_identity(bb);
		return;
	}

	bb->commit = 0;
	__asm__ volatile("fence w, w" ::: "memory");
	bb->magic = EASYSTICK_CMD53_BB_MAGIC;
	bb->version = EASYSTICK_CMD53_BB_VERSION;
	bb->generation = EASYSTICK_CMD53_BB_SELFTEST_GEN;
	bb->event_seq = 1;
	bb->cmd_arg = 0x535E1F51u;
	bb->stage_request = EASYSTICK_CMD53_BB_SELFTEST_GEN;
	__asm__ volatile("fence w, w" ::: "memory");
	bb->commit = easystick_cmd53_bb_commit_word(EASYSTICK_CMD53_BB_SELFTEST_GEN, 1u);
	__asm__ volatile("fence w, w" ::: "memory");
	ets_printf("easystick-boot: CMD53_BB SELFTEST write; software restart\n");
	esp_restart();
}
#endif
#endif /* EASYSTICK_CMD53_BB_SELFTEST[_TORN] */
