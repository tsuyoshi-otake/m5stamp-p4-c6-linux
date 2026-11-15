// SPDX-License-Identifier: Apache-2.0 OR GPL-2.0-only
/*
 * M5Stack Tab5 display bring-up inside the boot shim.
 *
 * Tab5 units ship with one of two display+touch chips, ST7121 or ST7123,
 * and they need different init tables and vsync timings.  The stock BSP
 * (M5Tab5-UserDemo m5stack_tab5.c, bsp_display_new_with_handles_to_st7123,
 * which serves both chips) does:
 *   0. bsp_reset_tp(): pulse LCD_RST and TP_RST together, then read the
 *      touch controller's firmware byte at I2C 0x55 to learn the panel
 *      (1 = ST7121, 3 = ST7123); tab5_panel_reset() / tab5_detect_panel()
 *      in tab5_board.c do the same.  The unit this port was brought up on
 *      is an ST7121: its stock boot log says "Detected ST7121 touch
 *      controller (FW version: 1), using ST7121 display" and "Install LCD
 *      driver of ST7121".  (An earlier revision of this file drove it with
 *      the ST7123 driver and timings and the panel stayed black.)
 *   1. power the MIPI D-PHY from the P4 internal LDO VO3 at 2.5 V
 *   2. create a 2-lane DSI bus at 965 Mbps per lane (both chips)
 *   3. attach a DBI command channel and the driver for that chip: the
 *      ST7121 driver's built-in init table (the BSP passes init_cmds NULL),
 *      or the ST7123 vendor table carried next to its driver
 *      (disp_init_data_st7123.h)
 *   4. DPI in RGB565, 720x1280, 70 MHz dot clock, hsync 2/40/40, vsync
 *      pulse/back/front 20/24/200 on ST7121 and 2/8/220 on ST7123
 *   5. panel reset (software, 120 ms), init table, display on, and only
 *      then the backlight
 *
 * Scan-out after the jump to Linux
 * -------------------------------
 * ESP-IDF's DPI panel driver feeds the DSI bridge through one DW-GDMA
 * channel programmed with a single full-frame link-list item (LLI).
 * Measured on the board (P4 v1.3):
 *
 *   - There is no hardware free-run: with the CPU's interrupts off the
 *     channel stalls after one frame (chen 0, int_st0 latches
 *     DMA_TFR_DONE).  The hardware clears the LLI valid bit in memory
 *     on every block end; IDF's trans-done callback re-marks it and
 *     re-enables the channel each frame, and the Linux
 *     esp32p4-dsi-scanout driver does the same in IRQ context.
 *   - A stalled channel re-arms cleanly (valid+last on the item through
 *     the uncached alias, llp0, chen) and the stream resumes at the
 *     same rate -- a handoff gap is recoverable, so the panel only
 *     holds the last frame between the jump and the driver's probe.
 *   - Channel registers reject writes while the channel is enabled
 *     (int_sig_ena0 write ignored, llp0 store bus-faults), so every
 *     register touch below happens only after chen reads 0.
 *   - llp0 reads back 0x1 while the channel runs (it holds the NEXT
 *     item), so the item is found by content: the 64-byte-aligned
 *     record in internal SRAM with sar == framebuffer and
 *     dar == 0x50105000 (scanout_find_lli).
 *
 * The panic Linux had after the jump is explained by the handoff, not
 * by the stream: IDF vectors every CPU interrupt line (clicintattr
 * shv=1, cpu_start.c) and leaves the DW-GDMA (src 24) and DSI-bridge
 * (src 86) sources routed at the interrupt matrix with their CLIC
 * slots enabled.  With the channel stalled the done status latches and
 * holds the line; the first MIE-raising moment in Linux dispatches it
 * through mtvt into the shim's dead vector table -- IDF code runs on
 * Linux's stack and double-panics.  tab5_display_status() therefore
 * detaches both sources and quiesces the channel before the jump; the
 * kernel keeper re-establishes the GDMA route when it claims
 * dsi-scanout@50081000 (interrupts-extended <&clic0 25 24 ...>).
 */

#include <inttypes.h>
#include <stdint.h>
#include <stdbool.h>
#include <string.h>

#include "esp_attr.h"
#include "esp_check.h"
#include "esp_log.h"
#include "esp_ldo_regulator.h"
#include "esp_lcd_mipi_dsi.h"
#include "esp_lcd_panel_ops.h"
#include "esp_lcd_st7121.h"
#include "esp_lcd_st7123.h"
#include "disp_init_data_st7123.h"
#include "driver/gpio.h"
#include "driver/ledc.h"
#include "esp_heap_caps.h"
#include "esp_rom_sys.h"
#include "riscv/csr.h"
#include "riscv/rv_utils.h"
#include "hal/cache_hal.h"
#include "hal/cache_ll.h"
#include "hal/dw_gdma_ll.h"
#include "hal/mipi_dsi_brg_ll.h"
#include "soc/clic_reg.h"
#include "soc/soc.h"
#include "soc/dw_gdma_struct.h"
#include "soc/interrupt_core0_reg.h"
#include "soc/mipi_dsi_bridge_struct.h"
#include "tab5_board.h"
#include "tab5_display.h"
#include "tab5_layout.h"

static const char *TAG = "tab5-disp";

#define LCD_H_RES 720
#define LCD_V_RES 1280
#define LCD_DSI_LANES 2
/* Both ST712x panels run the DSI link at 965 Mbps per lane in the stock
 * BSP (bsp_display_new_with_handles_to_st7123: "ST7123/ST7121 lane
 * bitrate").  An earlier revision of this file used 1000 Mbps, which is
 * esp-bsp's default for the other Tab5 panels. */
#define LCD_DSI_LANE_MBPS 965
#define LCD_DPI_MHZ 70
#define LCD_DPHY_LDO_CHAN 3
#define LCD_DPHY_LDO_MV 2500

/* Per-chip vsync timing, straight from the BSP (is_st7121 ? a : b). */
struct panel_profile {
	const char *name;
	uint16_t vsync_pulse;
	uint16_t vsync_back;
	uint16_t vsync_front;
};
static const struct panel_profile profile_st7121 = {
	.name = "ST7121", .vsync_pulse = 20, .vsync_back = 24, .vsync_front = 200,
};
static const struct panel_profile profile_st7123 = {
	.name = "ST7123", .vsync_pulse = 2, .vsync_back = 8, .vsync_front = 220,
};

/* Backlight: stock BSP uses LEDC low-speed timer 0 at 5 kHz on GPIO22
 * with LEDC_AUTO_CLK (12-bit duty, channel 1, 100% after display init).
 * AUTO on P4 resolves through the global-clock path; this pins XTAL
 * (40 MHz, never gated by the Linux port) so the PWM survives both the
 * Linux handoff and later clock changes.  The stock 100% together with
 * the speaker amp and the C6 access point browns out on plain USB power
 * (live-log.txt), so the shim keeps a conservative level: ~30% is bright
 * enough to judge the first picture in a lit room. */
#define LCD_BL_GPIO 22
#define LCD_BL_DUTY_RES LEDC_TIMER_10_BIT
#define LCD_BL_FREQ_HZ 5000
#define LCD_BL_DUTY 307 /* ~30% of 1024 */
#define LCD_BL_FADE_MS 200

/* Static banner drawn before Linux is loaded.  The framebuffer is RGB565
 * little-endian; a solid fill keeps this independent of font assets. */
#define FB_STRIDE_PX (LCD_H_RES)
#define FB_BYTES ((uint32_t)LCD_H_RES * LCD_V_RES * 2u)

/* DW-GDMA register facts the handoff depends on. */
#define DWGDMA_CH_COUNT 4
#define DWGDMA_DAR_DSI_FIFO 0x50105000u /* MIPI_DSI_BRG_MEM_BASE */
#define DWGDMA_INT_BLOCK_TFR_DONE (1u << 0)
#define DWGDMA_INT_DMA_TFR_DONE (1u << 1)
#define DWGDMA_INT_LLI_INVALID_ERR (1u << 13)
#define DWGDMA_INT_CH_DISABLED (1u << 30)
#define DWGDMA_INT_CH_ABORTED (1u << 31)
#define DWGDMA_LLI_CTRL_VALID (1u << 31)
#define DWGDMA_LLI_CTRL_LAST (1u << 30)

static dw_gdma_dev_t *const gdma = &DW_GDMA;
static int s_scanout_chan = -1;
static uint32_t s_scanout_lli_pa;

/* Kernel window + gap + DTB window, 16 MiB from the start of PSRAM. */
static uint8_t s_load_windows[(TAB5_DTB_LOAD_PA - TAB5_KERNEL_LOAD_PA) +
			      TAB5_LOAD_RESERVE_BYTES]
	EXT_RAM_NOINIT_ATTR __attribute__((aligned(64)));

static void draw_splash(uint16_t *fb)
{
	/* A test card that says something even without text: a dark blue
	 * field; a red | green | blue bar across the top (left to right, so a
	 * swapped colour order shows at once); a white band under it; and a
	 * 4-pixel white frame on the very edge of the 720x1280 portrait
	 * panel (a cropped or shifted picture shows as a missing frame side). */
	for (uint32_t i = 0; i < LCD_H_RES * LCD_V_RES; ++i)
		fb[i] = 0x000fu;
	for (uint32_t y = 40; y < 120; ++y) {
		for (uint32_t x = 40; x < LCD_H_RES - 40; ++x) {
			uint32_t third = (x - 40) * 3 / (LCD_H_RES - 80);
			fb[y * FB_STRIDE_PX + x] = third == 0 ? 0xf800u :
						   third == 1 ? 0x07e0u : 0x001fu;
		}
	}
	for (uint32_t y = 140; y < 170; ++y) {
		for (uint32_t x = 40; x < LCD_H_RES - 40; ++x)
			fb[y * FB_STRIDE_PX + x] = 0xffffu;
	}
	for (uint32_t y = 0; y < LCD_V_RES; ++y) {
		for (uint32_t x = 0; x < LCD_H_RES; ++x) {
			if (y < 4 || y >= LCD_V_RES - 4 || x < 4 ||
			    x >= LCD_H_RES - 4)
				fb[y * FB_STRIDE_PX + x] = 0xffffu;
		}
	}
	cache_hal_writeback_addr((uint32_t)(uintptr_t)fb, FB_BYTES);
}

static bool fb_collides(uint32_t pa, uint32_t bytes)
{
	for (size_t i = 0;
	     i < sizeof(tab5_fb_guard_regions) / sizeof(tab5_fb_guard_regions[0]);
	     ++i) {
		uint32_t r0 = tab5_fb_guard_regions[i][0];
		uint32_t r1 = r0 + tab5_fb_guard_regions[i][1];
		if (pa < r1 && pa + bytes > r0)
			return true;
	}
	return false;
}

/* Find the GDMA channel the DPI driver allocated: it is the only one
 * whose destination is the DSI-bridge FIFO window. */
static int scanout_find_channel(void)
{
	for (int i = 0; i < DWGDMA_CH_COUNT; ++i) {
		if (gdma->ch[i].dar0.val == DWGDMA_DAR_DSI_FIFO) {
			s_scanout_chan = i;
			return i;
		}
	}
	return -1;
}

/* The channel's llp0 reads 0 while the block runs (it holds the NEXT item),
 * so the item is found by content instead: the one 64-byte-aligned record
 * in internal RAM with sar == framebuffer and dar == the DSI FIFO. */
#define SRAM_SCAN_LO 0x4ff00000u
#define SRAM_SCAN_HI 0x4ff80000u
static uint32_t scanout_find_lli(uint32_t fb_pa)
{
	for (uint32_t pa = SRAM_SCAN_LO; pa < SRAM_SCAN_HI; pa += 64) {
		const volatile uint32_t *w = (const volatile uint32_t *)
			CACHE_LL_L2MEM_NON_CACHE_ADDR(pa);
		if (w[0] == fb_pa && w[2] == DWGDMA_DAR_DSI_FIFO)
			return pa;
	}
	return 0;
}

/* Re-arm the scan-out channel exactly the way the IDF trans-done
 * callback does: re-mark the LLI valid+last, re-point llp0 at it, and
 * re-enable the channel.  Called by the self-test and documented as the
 * contract the kernel DSI-scanout driver implements. */
static void scanout_rearm(int ch)
{
	/* Write through the uncached alias, as dw_gdma.c does (items_nc):
	 * the DMA reads the item from RAM, not from the L1 data cache. */
	volatile dw_gdma_link_list_item_t *lli =
		(void *)CACHE_LL_L2MEM_NON_CACHE_ADDR(s_scanout_lli_pa);
	lli->ctrl_hi.val |= DWGDMA_LLI_CTRL_VALID | DWGDMA_LLI_CTRL_LAST;
	gdma->ch[ch].llp0.loc0 = s_scanout_lli_pa >> 6;
	dw_gdma_ll_channel_enable(gdma, ch, true);
}

/*
 * Three-fact serial self-test.  The channel's llp0 can only be written
 * while the channel is disabled (a store while it runs is a bus fault
 * on this SoC), so the test never re-arms a running channel.
 *
 *   1. live rate  -- source-pointer wraps while the IDF trans-done ISR
 *      re-arms each frame;
 *   2. irq-off    -- with mstatus.MIE clear the callback dies with the
 *      CPU and the channel must stall inside a frame: there is no
 *      hardware free-run;
 *   3. re-arm     -- a stalled channel takes the kernel-driver sequence
 *      and the stream resumes.
 *
 * Returns true when the stall and the resumed stream behave as
 * designed.
 */
static uint32_t gdma_sample_wraps(int ch, uint32_t ms, uint32_t *moved)
{
	volatile dmac_channel_reg_t *c = &gdma->ch[ch];
	uint32_t last = c->sar0.val, wraps = 0, mv = 0;

	for (uint32_t i = 0; i < ms * 4; ++i) {
		uint32_t now = c->sar0.val;
		if (now != last)
			++mv;
		if (now < last && (last - now) > (FB_BYTES / 2))
			++wraps; /* jumped back to the frame start */
		last = now;
		esp_rom_delay_us(250);
	}
	if (moved)
		*moved = mv;
	return wraps;
}

static bool scanout_self_test(int ch)
{
	volatile dmac_channel_reg_t *c = &gdma->ch[ch];
	const uint32_t frame_us = 17000;
	uint32_t moved, wraps_live, wraps_b, st_a;
	uint32_t chen_a;
	bool stalled, ok;

	wraps_live = gdma_sample_wraps(ch, 300, &moved);

	unsigned long mie = RV_CLEAR_CSR(mstatus, MSTATUS_MIE) & MSTATUS_MIE;
	esp_rom_delay_us(2 * frame_us);
	chen_a = gdma->chen0.val & (1u << ch);
	gdma_sample_wraps(ch, 40, &moved);
	st_a = c->int_st0.val;
	stalled = (moved == 0) && (chen_a == 0);
	if (stalled) {
		/* Channel disabled: register writes are legal now. */
		c->int_clr0.val = 0xffffffffu;
		scanout_rearm(ch);
	}
	if (mie)
		RV_SET_CSR(mstatus, MSTATUS_MIE);
	/* The console was starved while interrupts were off; let it drain. */
	esp_rom_delay_us(50000);
	ESP_LOGI(TAG, "self-test: %" PRIu32 " frames/300ms with ISR; "
		 "irq-off %s (chen=%d st0=0x%08" PRIx32 "); re-arm %s",
		 wraps_live, stalled ? "stall" : "NO STALL",
		 chen_a ? 1 : 0, st_a,
		 stalled ? "issued" : "skipped");

	wraps_b = gdma_sample_wraps(ch, 600, &moved);
	ok = stalled && wraps_b >= 30;
	ESP_LOGI(TAG, "self-test: %" PRIu32 " frames/600ms after re-arm "
		 "-> %s", wraps_b,
		 ok ? "stream healthy" : "STREAM NOT HEALTHY");
	return ok;
}

static void backlight_init(void)
{
	ledc_timer_config_t timer = {
		.speed_mode = LEDC_LOW_SPEED_MODE,
		.duty_resolution = LCD_BL_DUTY_RES,
		.timer_num = LEDC_TIMER_0,
		.freq_hz = LCD_BL_FREQ_HZ,
		.clk_cfg = LEDC_USE_XTAL_CLK,
	};
	ledc_channel_config_t chan = {
		.gpio_num = LCD_BL_GPIO,
		.speed_mode = LEDC_LOW_SPEED_MODE,
		.channel = LEDC_CHANNEL_0,
		.timer_sel = LEDC_TIMER_0,
		.duty = 0,
		.hpoint = 0,
	};
	if (ledc_timer_config(&timer) != ESP_OK ||
	    ledc_channel_config(&chan) != ESP_OK) {
		ESP_LOGW(TAG, "backlight LEDC setup failed; GPIO%d stays low",
			 LCD_BL_GPIO);
		return;
	}
	ledc_set_duty(LEDC_LOW_SPEED_MODE, LEDC_CHANNEL_0, LCD_BL_DUTY);
	ledc_update_duty(LEDC_LOW_SPEED_MODE, LEDC_CHANNEL_0);
	ESP_LOGI(TAG, "backlight on: GPIO%d 5 kHz/10-bit XTAL duty %u/1024",
		 LCD_BL_GPIO, (unsigned)LCD_BL_DUTY);
}

/*
 * Last display step before the jump: make the hand-off silent.
 *
 * Every IDF-allocated CPU line is vectored (clicintattr shv=1) and the
 * kernel's CLIC driver only touches the slots it claims, so any display
 * source still routed+enabled at the jump would dispatch through mtvt
 * into the dead IDF vector table the moment Linux raises MIE -- the
 * board showed exactly that (IDF panic text after "bootconsole
 * enabled").  Sequence, all with mstatus.MIE clear:
 *
 *   1. detach the DW-GDMA (src 24) and DSI-bridge (src 86) sources at
 *      the interrupt matrix so they cannot reach any CLIC slot;
 *   2. clear int-enable and pending on the two CLIC slots the IDF
 *      routes had pointed at;
 *   3. mask and clear the DSI bridge's own interrupt;
 *   4. wait for the GDMA channel to stall (no re-armer can run any
 *      more; one frame worst case) -- channel registers are only
 *      writable while chen is 0, so masking and clearing the channel's
 *      interrupt status comes last;
 *   5. read back and print, so the serial log states the quiesced
 *      state Linux inherits.
 *
 * The kernel esp32p4-dsi-scanout driver re-establishes the GDMA route
 * for its own slot when it probes (clic_domain_map writes
 * intmtx[source] = slot), then re-arms the stalled channel.
 */
void tab5_display_status(void)
{
	if (s_scanout_chan < 0) {
		ESP_LOGI(TAG, "scanout: no DW-GDMA channel tracked");
		return;
	}
	volatile dmac_channel_reg_t *c = &gdma->ch[s_scanout_chan];
	unsigned long mie = RV_CLEAR_CSR(mstatus, MSTATUS_MIE) & MSTATUS_MIE;

	uint32_t gdma_route = REG_READ(INTERRUPT_CORE0_GDMA_INT_MAP_REG);
	uint32_t brg_route = REG_READ(INTERRUPT_CORE0_DSI_BRIDGE_INT_MAP_REG);
	uint32_t gdma_slot = gdma_route & 0x3f;
	uint32_t brg_slot = brg_route & 0x3f;
	REG_WRITE(INTERRUPT_CORE0_GDMA_INT_MAP_REG, 0);
	REG_WRITE(INTERRUPT_CORE0_DSI_BRIDGE_INT_MAP_REG, 0);
	for (uint32_t slot = 0; slot < 2; ++slot) {
		uint32_t s = slot ? brg_slot : gdma_slot;
		if (!s)
			continue; /* source was never routed; leave line 0 alone */
		*(volatile uint8_t *)BYTE_CLIC_INT_IE_REG(s) = 0;
		*(volatile uint8_t *)BYTE_CLIC_INT_IP_REG(s) = 0;
	}
	MIPI_DSI_BRIDGE.int_ena.val = 0;
	MIPI_DSI_BRIDGE.int_clr.val = 0xffffffffu;

	uint32_t wait_us = 0;
	while ((gdma->chen0.val & (1u << s_scanout_chan)) && wait_us < 40000) {
		esp_rom_delay_us(500);
		wait_us += 500;
	}
	uint32_t chen_rb = gdma->chen0.val;
	if (chen_rb & (1u << s_scanout_chan)) {
		/* Channel still enabled: touching its registers would
		 * fault.  The sources are detached so nothing reaches the
		 * CPU, but the tail clear is skipped. */
		ESP_LOGW(TAG, "handoff: ch%d still enabled after %" PRIu32
			 " us; registers left alone", s_scanout_chan, wait_us);
	} else {
		c->int_sig_ena0.val = 0;
		c->int_sig_ena1.val = 0;
		c->int_clr0.val = 0xffffffffu;
	}
	uint32_t st_rb = c->int_st0.val;
	uint32_t brg_st = MIPI_DSI_BRIDGE.int_st.val;
	uint32_t gdma_map_rb = REG_READ(INTERRUPT_CORE0_GDMA_INT_MAP_REG);
	uint32_t brg_map_rb = REG_READ(INTERRUPT_CORE0_DSI_BRIDGE_INT_MAP_REG);

	if (mie)
		RV_SET_CSR(mstatus, MSTATUS_MIE);
	esp_rom_delay_us(50000); /* console drain after the irq-off window */

	ESP_LOGI(TAG, "handoff: gdma route %u->%u brg route %u->%u, "
		 "slots %u/%u IE+IP cleared, ch%d chen0=0x%08" PRIx32
		 " st0=0x%08" PRIx32 " brg_st=0x%08" PRIx32
		 " (Linux dsi-scanout re-arms)",
		 gdma_route & 0x3f, gdma_map_rb & 0x3f,
		 brg_route & 0x3f, brg_map_rb & 0x3f,
		 gdma_slot, brg_slot, s_scanout_chan, chen_rb, st_rb, brg_st);
}

/* Address of the scan-out LLI in internal SRAM, found by content; 0 if
 * the stream never came up.  Written into the DTB for the kernel
 * driver. */
uint32_t tab5_scanout_lli(void)
{
	return s_scanout_lli_pa;
}

/* DW-GDMA channel streaming into the DSI bridge; -1 when unknown. */
int tab5_scanout_chan(void)
{
	return s_scanout_chan;
}

void tab5_display_init(struct tab5_fb *out)
{
	/* The kernel and DTB load windows are held by a linker-placed array at
	 * the very start of PSRAM, so the PSRAM heap (and its metadata) begins
	 * above them and nothing the shim allocates is ever overwritten by the
	 * kernel/DTB copy.  Fail closed if the linker did not put it there. */
	if ((uintptr_t)s_load_windows != TAB5_KERNEL_LOAD_PA ||
	    sizeof(s_load_windows) < (TAB5_DTB_LOAD_PA - TAB5_KERNEL_LOAD_PA) +
				     TAB5_LOAD_RESERVE_BYTES) {
		ESP_LOGW(TAG, "load-window array at %p (%u bytes), expected 0x%08x; "
			 "display skipped", s_load_windows,
			 (unsigned)sizeof(s_load_windows), TAB5_KERNEL_LOAD_PA);
		return;
	}
	ESP_LOGI(TAG, "load windows held at %p (%u bytes)", s_load_windows,
		 (unsigned)sizeof(s_load_windows));

	/* Same first two steps as the stock BSP: reset the display+touch
	 * chip, then ask the touch controller which chip it is. */
	tab5_panel_reset();
	uint8_t touch_fw = 0;
	const enum tab5_panel kind = tab5_detect_panel(&touch_fw);
	if (kind == TAB5_PANEL_NONE) {
		ESP_LOGW(TAG, "no ST7121/ST7123 touch controller at 0x55 (the "
			 "ILI9881C/GT911 panel variant is not supported); "
			 "display skipped");
		return;
	}
	const struct panel_profile *prof =
		kind == TAB5_PANEL_ST7121 ? &profile_st7121 : &profile_st7123;
	ESP_LOGI(TAG, "panel %s (touch FW byte %u): %u Mbps x %d lanes, "
		 "vsync %u/%u/%u", prof->name, touch_fw,
		 (unsigned)LCD_DSI_LANE_MBPS, LCD_DSI_LANES,
		 (unsigned)prof->vsync_pulse, (unsigned)prof->vsync_back,
		 (unsigned)prof->vsync_front);

	esp_ldo_channel_handle_t ldo = NULL;
	esp_lcd_dsi_bus_handle_t dsi = NULL;
	esp_lcd_panel_io_handle_t io = NULL;
	esp_lcd_panel_handle_t panel = NULL;

	esp_ldo_channel_config_t ldo_cfg = {
		.chan_id = LCD_DPHY_LDO_CHAN,
		.voltage_mv = LCD_DPHY_LDO_MV,
	};
	if (esp_ldo_acquire_channel(&ldo_cfg, &ldo) != ESP_OK) {
		ESP_LOGW(TAG, "DPHY LDO acquire failed; display skipped");
		return;
	}

	esp_lcd_dsi_bus_config_t bus_cfg = {
		.bus_id = 0,
		.num_data_lanes = LCD_DSI_LANES,
		.phy_clk_src = 0, /* BSP: let the driver choose the default */
		.lane_bit_rate_mbps = LCD_DSI_LANE_MBPS,
	};
	if (esp_lcd_new_dsi_bus(&bus_cfg, &dsi) != ESP_OK) {
		ESP_LOGW(TAG, "DSI bus failed; display skipped");
		return;
	}

	esp_lcd_dbi_io_config_t dbi_cfg = {
		.virtual_channel = 0,
		.lcd_cmd_bits = 8,
		.lcd_param_bits = 8,
	};
	if (esp_lcd_new_panel_io_dbi(dsi, &dbi_cfg, &io) != ESP_OK) {
		ESP_LOGW(TAG, "DBI io failed; display skipped");
		return;
	}

	const esp_lcd_dpi_panel_config_t dpi_cfg = {
		.virtual_channel = 0,
		.dpi_clk_src = MIPI_DSI_DPI_CLK_SRC_DEFAULT,
		.dpi_clock_freq_mhz = LCD_DPI_MHZ,
		.in_color_format = LCD_COLOR_FMT_RGB565,
		.num_fbs = 1,
		.video_timing = {
			.h_size = LCD_H_RES,
			.v_size = LCD_V_RES,
			.hsync_back_porch = 40,
			.hsync_pulse_width = 2,
			.hsync_front_porch = 40,
			.vsync_back_porch = prof->vsync_back,
			.vsync_pulse_width = prof->vsync_pulse,
			.vsync_front_porch = prof->vsync_front,
		},
	};
	esp_lcd_panel_dev_config_t panel_cfg = {
		.reset_gpio_num = GPIO_NUM_NC, /* software reset; LCD_RST was
						* pulsed through expander 0x43 P4 */
		.flags = {
			.reset_active_high = 0,
		},
		.rgb_ele_order = LCD_RGB_ELEMENT_ORDER_RGB,
		.bits_per_pixel = 16,
	};
	esp_err_t err;
	if (kind == TAB5_PANEL_ST7121) {
		/* init_cmds NULL: the driver's built-in table, as the BSP does. */
		const st7121_vendor_config_t vendor_cfg = {
			.init_cmds = NULL,
			.init_cmds_size = 0,
			.mipi_config = {
				.dsi_bus = dsi,
				.dpi_config = &dpi_cfg,
			},
		};
		panel_cfg.vendor_config = (void *)&vendor_cfg;
		err = esp_lcd_new_panel_st7121(io, &panel_cfg, &panel);
	} else {
		const st7123_vendor_config_t vendor_cfg = {
			.init_cmds = disp_init_data_st7123,
			.init_cmds_size = sizeof(disp_init_data_st7123) /
					  sizeof(disp_init_data_st7123[0]),
			.mipi_config = {
				.dsi_bus = dsi,
				.dpi_config = &dpi_cfg,
			},
		};
		panel_cfg.vendor_config = (void *)&vendor_cfg;
		err = esp_lcd_new_panel_st7123(io, &panel_cfg, &panel);
	}
	if (err != ESP_OK) {
		ESP_LOGW(TAG, "%s panel failed (%s); display skipped",
			 prof->name, esp_err_to_name(err));
		return;
	}
	if (esp_lcd_panel_reset(panel) != ESP_OK ||
	    esp_lcd_panel_init(panel) != ESP_OK ||
	    esp_lcd_panel_disp_on_off(panel, true) != ESP_OK) {
		ESP_LOGW(TAG, "%s init sequence failed; display skipped",
			 prof->name);
		return;
	}
	ESP_LOGI(TAG, "%s init sequence done", prof->name);

	void *fb = NULL;
	if (esp_lcd_dpi_panel_get_frame_buffer(panel, 1, &fb) != ESP_OK || !fb) {
		ESP_LOGW(TAG, "framebuffer query failed; display skipped");
		return;
	}
	if (fb_collides((uint32_t)(uintptr_t)fb, FB_BYTES)) {
		ESP_LOGE(TAG, "framebuffer @%p overlaps a Linux load region; "
			 "tearing down the panel and booting headless", fb);
		esp_lcd_panel_del(panel);
		return;
	}
	draw_splash((uint16_t *)fb);

	out->pa = (uint32_t)(uintptr_t)fb;
	out->bytes = FB_BYTES;
	out->ok = true;

	s_scanout_lli_pa = scanout_find_lli(out->pa);
	ESP_LOGI(TAG, "scanout LLI at 0x%08" PRIx32, s_scanout_lli_pa);
	if (scanout_find_channel() < 0 || !s_scanout_lli_pa) {
		ESP_LOGW(TAG, "DW-GDMA scanout channel/LLI not found; "
			 "stream will die at the IDF callback boundary");
	} else if (scanout_self_test(s_scanout_chan)) {
		backlight_init();
	} else {
		ESP_LOGE(TAG, "scanout self-test failed; backlight stays off "
			 "and the picture may freeze at handoff");
	}
	ESP_LOGI(TAG, "%s up: fb @0x%08" PRIx32 " (%" PRIu32 " bytes), "
		 "stream left running for Linux simple-framebuffer",
		 prof->name, out->pa, out->bytes);
}
