// SPDX-License-Identifier: Apache-2.0 OR GPL-2.0-only
/*
 * #25 CAMERA_ORACLE: a known-good one-frame capture through IDF's own
 * esp_driver_cam CSI controller, before Linux sees the hardware.  This
 * is the reference the Linux camera driver gets diffed against, the
 * same role AUDIO_ORACLE played for I2S.  Built only when
 * CAMERA_ORACLE=1 is in the environment (CMakeLists refuses it for
 * non-Tab5 boards); never in a release shim.
 *
 * Flow: release CAM_RST (expander 0x43 P6), verify the SC2356/SC202CS
 * chip ID over SCCB (internal I2C0 @ 0x36, 16-bit register
 * addressing), program the published esp-video-components 1-lane
 * 24 MHz-input 1280x720 RAW8 table, capture one frame into PSRAM with
 * esp_cam_new_csi_ctlr, print Bayer stats + the full CSI/bridge/PHY
 * /DW-GDMA/CLKRST register state, then render a grey downscale into
 * the panel framebuffer so the capture is visible on the screen for a
 * few seconds before Linux boots.
 *
 * Hardware facts (linux/tab5/camera-bringup.md): XCLK is 24 MHz the
 * SoC must drive on GPIO36 (M5 BSP: bsp_cam_osc_init); MIPI-CSI 1 data
 * lane; CAM_RST P6 is the only power/reset line.
 */
#include <inttypes.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "esp_attr.h"
#include "esp_cache.h"
#include "esp_check.h"
#include "esp_err.h"
#include "esp_heap_caps.h"
#include "esp_ldo_regulator.h"
#include "esp_log.h"
#include "esp_rom_sys.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "driver/i2c_master.h"

#include "esp_cam_ctlr.h"
#include "esp_cam_ctlr_csi.h"

#include "driver/isp_core.h"

#include "esp_private/esp_clk_tree_common.h"
#include "esp_private/periph_ctrl.h"

#include "hal/cam_ll.h"
#include "esp_rom_gpio.h"

#include "soc/gpio_sig_map.h"
#include "soc/isp_struct.h"
#include "soc/mipi_csi_host_struct.h"
#include "soc/mipi_csi_bridge_struct.h"
#include "soc/dw_gdma_struct.h"
#include "soc/hp_sys_clkrst_struct.h"
#include "soc/mipi_csi_periph.h"

#include "camera_oracle.h"
#include "tab5_board.h"

static const char *TAG = "cam-oracle";

#define SC202CS_ADDR 0x36u
#define SC202CS_PID 0xeb52u

#define CAM_H_RES 1280u
#define CAM_V_RES 720u
#define CAM_LANES 1u
#define CAM_LANE_MBPS 576u
#define FRAME_BYTES (CAM_H_RES * CAM_V_RES) /* RAW8: 1 byte/pixel */

#define CAM_LDO_CHAN 3
#define CAM_LDO_MV 2500 /* same channel/voltage the DSI DPHY uses */

/* M5Tab5-UserDemo's BSP drives the sensor XCLK from the SoC: 24 MHz
 * on GPIO36 via LEDC (bsp_cam_osc_init).  camera-bringup.md assumed a
 * dedicated oscillator made GPIO36 a dead end, but that was schematic-
 * only; SCCB works without XCLK while the sensor PLL/MIPI TX does not.
 * TIMER_1/CH1: the display backlight owns TIMER_0/CH0 at 5 kHz. */
#define CAM_XCLK_GPIO 36
#define CAM_XCLK_HZ 24000000u

#define HOLD_ON_PANEL_MS 12000u

/* sc202cs_mipi_1lane_24Minput_1280x720_raw8_30fps.h from
 * espressif/esp-video-components verbatim (A16V8 regs; 0xfffe marks a
 * delay in ms, 0xffff ends the table). */
static const uint16_t sensor_init[][2] = {
	{0x0103, 0x01},
	{0x0100, 0x00},
	{0x36e9, 0x80},
	{0x36ea, 0x06},
	{0x36eb, 0x0a},
	{0x36ec, 0x01},
	{0x36ed, 0x18},
	{0x36e9, 0x24},
	{0x301f, 0x18},
	{0x3031, 0x08},
	{0x3037, 0x00},
	{0x3200, 0x00},
	{0x3201, 0xa0},
	{0x3202, 0x00},
	{0x3203, 0xf0},
	{0x3204, 0x05},
	{0x3205, 0xa7},
	{0x3206, 0x03},
	{0x3207, 0xc7},
	{0x3208, 0x05},
	{0x3209, 0x00},
	{0x320a, 0x02},
	{0x320b, 0xd0},
	{0x3210, 0x00},
	{0x3211, 0x04},
	{0x3212, 0x00},
	{0x3213, 0x04},
	{0x3301, 0xff},
	{0x3304, 0x68},
	{0x3306, 0x40},
	{0x3308, 0x08},
	{0x3309, 0xa8},
	{0x330b, 0xd0},
	{0x330c, 0x18},
	{0x330d, 0xff},
	{0x330e, 0x20},
	{0x331e, 0x59},
	{0x331f, 0x99},
	{0x3333, 0x10},
	{0x335e, 0x06},
	{0x335f, 0x08},
	{0x3364, 0x1f},
	{0x337c, 0x02},
	{0x337d, 0x0a},
	{0x338f, 0xa0},
	{0x3390, 0x01},
	{0x3391, 0x03},
	{0x3392, 0x1f},
	{0x3393, 0xff},
	{0x3394, 0xff},
	{0x3395, 0xff},
	{0x33a2, 0x04},
	{0x33ad, 0x0c},
	{0x33b1, 0x20},
	{0x33b3, 0x38},
	{0x33f9, 0x40},
	{0x33fb, 0x48},
	{0x33fc, 0x0f},
	{0x33fd, 0x1f},
	{0x349f, 0x03},
	{0x34a6, 0x03},
	{0x34a7, 0x1f},
	{0x34a8, 0x38},
	{0x34a9, 0x30},
	{0x34ab, 0xd0},
	{0x34ad, 0xd8},
	{0x34f8, 0x1f},
	{0x34f9, 0x20},
	{0x3630, 0xa0},
	{0x3631, 0x92},
	{0x3632, 0x64},
	{0x3633, 0x43},
	{0x3637, 0x49},
	{0x363a, 0x85},
	{0x363c, 0x0f},
	{0x3650, 0x31},
	{0x3670, 0x0d},
	{0x3674, 0xc0},
	{0x3675, 0xa0},
	{0x3676, 0xa0},
	{0x3677, 0x92},
	{0x3678, 0x96},
	{0x3679, 0x9a},
	{0x367c, 0x03},
	{0x367d, 0x0f},
	{0x367e, 0x01},
	{0x367f, 0x0f},
	{0x3698, 0x83},
	{0x3699, 0x86},
	{0x369a, 0x8c},
	{0x369b, 0x94},
	{0x36a2, 0x01},
	{0x36a3, 0x03},
	{0x36a4, 0x07},
	{0x36ae, 0x0f},
	{0x36af, 0x1f},
	{0x36bd, 0x22},
	{0x36be, 0x22},
	{0x36bf, 0x22},
	{0x36d0, 0x01},
	{0x370f, 0x02},
	{0x3721, 0x6c},
	{0x3722, 0x8d},
	{0x3725, 0xc5},
	{0x3727, 0x14},
	{0x3728, 0x04},
	{0x37b7, 0x04},
	{0x37b8, 0x04},
	{0x37b9, 0x06},
	{0x37bd, 0x07},
	{0x37be, 0x0f},
	{0x3901, 0x02},
	{0x3903, 0x40},
	{0x3905, 0x8d},
	{0x3907, 0x00},
	{0x3908, 0x41},
	{0x391f, 0x41},
	{0x3933, 0x80},
	{0x3934, 0x02},
	{0x3937, 0x6f},
	{0x393a, 0x01},
	{0x393d, 0x01},
	{0x393e, 0xc0},
	{0x39dd, 0x41},
	{0x3e00, 0x00},
	{0x3e01, 0x3d},
	{0x3e02, 0xc0},
	{0x3e09, 0x00},
	{0x4509, 0x28},
	{0x450d, 0x61},
	{0xffff, 0x00},
};

static inline volatile uint32_t *reg32(uint32_t addr)
{
	return (volatile uint32_t *)(uintptr_t)addr;
}

static void dump_range(const char *name, uint32_t base, uint32_t bytes)
{
	for (uint32_t off = 0; off < bytes; off += 4u)
		ESP_LOGI(TAG, "R %s 0x%08" PRIx32 " = 0x%08" PRIx32, name,
			 base + off, *reg32(base + off));
}

static esp_err_t sccb_write(i2c_master_dev_handle_t dev, uint16_t reg,
			    uint8_t val)
{
	uint8_t buf[3] = { reg >> 8, reg & 0xff, val };
	return i2c_master_transmit(dev, buf, sizeof(buf), 100);
}

static esp_err_t sccb_read(i2c_master_dev_handle_t dev, uint16_t reg,
			   uint8_t *val)
{
	uint8_t addr[2] = { reg >> 8, reg & 0xff };
	return i2c_master_transmit_receive(dev, addr, sizeof(addr), val, 1,
					   100);
}

static bool IRAM_ATTR cam_get_new_trans(esp_cam_ctlr_handle_t handle,
					esp_cam_ctlr_trans_t *trans,
					void *user_data)
{
	esp_cam_ctlr_trans_t *want = (esp_cam_ctlr_trans_t *)user_data;
	trans->buffer = want->buffer;
	trans->buflen = want->buflen;
	return false;
}

static volatile bool s_frame_done;

static bool IRAM_ATTR cam_trans_finished(esp_cam_ctlr_handle_t handle,
					 esp_cam_ctlr_trans_t *trans,
					 void *user_data)
{
	esp_cam_ctlr_trans_t *want = (esp_cam_ctlr_trans_t *)user_data;
	(void)handle;
	want->received_size = trans->received_size;
	s_frame_done = true;
	return false;
}

static void frame_stats(const uint8_t *f, size_t n)
{
	uint64_t sum[4] = { 0 };
	uint32_t cnt[4] = { 0 };
	uint8_t mn[4] = { 0xff, 0xff, 0xff, 0xff };
	uint8_t mx[4] = { 0 };
	uint32_t hist[16] = { 0 };
	uint64_t quad[4] = { 0 };
	uint32_t qcnt[4] = { 0 };

	for (uint32_t y = 0; y < CAM_V_RES; ++y) {
		for (uint32_t x = 0; x < CAM_H_RES; ++x) {
			uint8_t v = f[y * CAM_H_RES + x];
			uint32_t cell = (y & 1u) * 2u + (x & 1u);
			sum[cell] += v;
			cnt[cell]++;
			if (v < mn[cell])
				mn[cell] = v;
			if (v > mx[cell])
				mx[cell] = v;
			hist[v >> 4]++;
			uint32_t q = (y >= CAM_V_RES / 2u) * 2u +
				     (x >= CAM_H_RES / 2u);
			quad[q] += v;
			qcnt[q]++;
		}
	}
	for (uint32_t i = 0; i < 4u; ++i)
		ESP_LOGI(TAG, "bayer cell (y%%2=%u,x%%2=%u): mean=%u min=%u "
			 "max=%u", i / 2u, i % 2u,
			 (unsigned)(cnt[i] ? sum[i] / cnt[i] : 0), mn[i],
			 mx[i]);
	ESP_LOGI(TAG, "hist(16 bins): %u %u %u %u %u %u %u %u %u %u %u %u "
		 "%u %u %u %u",
		 hist[0], hist[1], hist[2], hist[3], hist[4], hist[5],
		 hist[6], hist[7], hist[8], hist[9], hist[10], hist[11],
		 hist[12], hist[13], hist[14], hist[15]);
	for (uint32_t i = 0; i < 4u; ++i)
		ESP_LOGI(TAG, "quadrant %u mean=%u", i,
			 (unsigned)(qcnt[i] ? quad[i] / qcnt[i] : 0));
	(void)n;
}

static void dump_csi_regs(void)
{
	ESP_LOGI(TAG, "=== CAMERA_ORACLE register dump ===");
	dump_range("csihost", (uint32_t)(uintptr_t)&MIPI_CSI_HOST,
		   sizeof(csi_host_dev_t));
	dump_range("csibrg", (uint32_t)(uintptr_t)&MIPI_CSI_BRIDGE,
		   sizeof(csi_brg_dev_t));
	/* The DW_GDMA struct has unmapped reserved holes -- read only
	 * declared channel registers, never a byte range. */
	ESP_LOGI(TAG, "R dwgdma id0=0x%08" PRIx32 " compver=0x%08" PRIx32
		 " cfg0=0x%08" PRIx32 " chen0=0x%08" PRIx32,
		 DW_GDMA.id0.val, DW_GDMA.compver0.val,
		 DW_GDMA.cfg0.val, DW_GDMA.chen0.val);
	for (size_t c = 0;
	     c < sizeof(DW_GDMA.ch) / sizeof(DW_GDMA.ch[0]); ++c) {
		const dmac_channel_reg_t *r = &DW_GDMA.ch[c];
		ESP_LOGI(TAG,
			 "R dwgdma.ch%u sar=0x%08" PRIx32
			 " dar=0x%08" PRIx32 " bts=0x%08" PRIx32
			 " ctl0=0x%08" PRIx32 " cfg0=0x%08" PRIx32
			 " llp=0x%08" PRIx32 " st0=0x%08" PRIx32
			 " ist0=0x%08" PRIx32,
			 (unsigned)c, r->sar0.val, r->dar0.val,
			 r->block_ts0.val, r->ctl0.val, r->cfg0.val,
			 r->llp0.val, r->status0.val, r->int_st0.val);
	}
	ESP_LOGI(TAG, "R clkrst peri_clk_ctrl03 = 0x%08" PRIx32,
		 HP_SYS_CLKRST.peri_clk_ctrl03.val);
	ESP_LOGI(TAG, "R clkrst soc_clk_ctrl1 = 0x%08" PRIx32,
		 HP_SYS_CLKRST.soc_clk_ctrl1.val);
	ESP_LOGI(TAG, "R clkrst soc_clk_ctrl0 = 0x%08" PRIx32,
		 HP_SYS_CLKRST.soc_clk_ctrl0.val);
	ESP_LOGI(TAG, "R clkrst clk_en0 = 0x%08" PRIx32,
		 HP_SYS_CLKRST.clk_en0.val);
	/* The DPHY is written through a serial testbus (no readback);
	 * the only reg the HAL applies is 0x44 = hs_freq_sel<<1. */
	for (size_t i = 0; i < num_of_soc_mipi_csi_phy_pll_ranges; ++i) {
		if (CAM_LANE_MBPS >=
			    soc_mipi_csi_phy_pll_ranges[i].start_mbps &&
		    CAM_LANE_MBPS <=
			    soc_mipi_csi_phy_pll_ranges[i].end_mbps) {
			ESP_LOGI(TAG, "phy reg 0x44 (applied via testbus) = "
				 "0x%02x (hs_freq_sel=%u for %" PRIu32
				 " Mbps lane)",
				 soc_mipi_csi_phy_pll_ranges[i].hs_freq_range_sel
					 << 1,
				 soc_mipi_csi_phy_pll_ranges[i].hs_freq_range_sel,
				 (uint32_t)CAM_LANE_MBPS);
			break;
		}
	}
	/* ISP input module: the CSI->bridge data path on P4. */
	ESP_LOGI(TAG,
		 "R isp cntl=0x%08" PRIx32 " clk_en=0x%08" PRIx32
		 " frame_cfg=0x%08" PRIx32 " hsync_cnt=0x%08" PRIx32
		 " int_raw=0x%08" PRIx32 " int_st=0x%08" PRIx32,
		 ISP.cntl.val, ISP.clk_en.val, ISP.frame_cfg.val,
		 ISP.hsync_cnt.val, ISP.int_raw.val, ISP.int_st.val);
	ESP_LOGI(TAG,
		 "R clkrst peri_clk_ctrl25=0x%08" PRIx32
		 " peri_clk_ctrl26=0x%08" PRIx32
		 " hp_rst_en0=0x%08" PRIx32,
		 HP_SYS_CLKRST.peri_clk_ctrl25.val,
		 HP_SYS_CLKRST.peri_clk_ctrl26.val,
		 HP_SYS_CLKRST.hp_rst_en0.val);
}

/* Average each 2x2 Bayer cell to grey and draw it centered on the
 * 720x1280 RGB565 panel framebuffer (fb_va, fb_bytes as handed to
 * Linux).  The camera's 640x360 downscale sits in the middle of the
 * portrait panel. */
static void draw_on_panel(uint16_t *fb, uint32_t fb_bytes,
			  const uint8_t *frame)
{
	const uint32_t pw = 720, ph = 1280;
	const uint32_t gw = CAM_H_RES / 2u, gh = CAM_V_RES / 2u; /* 640x360 */
	const uint32_t x0 = (pw - gw) / 2u, y0 = (ph - gh) / 2u;
	uint8_t gmin = 0xff, gmax = 0;

	if (fb_bytes < pw * ph * 2u)
		return;
	/* Dark scenes sit in a tiny range (e.g. 15..49); stretch to full
	 * contrast so the live pixel noise is visible on the panel and in
	 * webcam photos. */
	for (uint32_t gy = 0; gy < gh; ++gy) {
		for (uint32_t gx = 0; gx < gw; ++gx) {
			const uint8_t *c =
				frame + (gy * 2u) * CAM_H_RES + gx * 2u;
			uint8_t v = (c[0] + c[1] + c[CAM_H_RES] +
				     c[CAM_H_RES + 1]) / 4u;
			if (v < gmin)
				gmin = v;
			if (v > gmax)
				gmax = v;
		}
	}
	const uint32_t span = (gmax > gmin) ? (uint32_t)(gmax - gmin) : 1u;
	memset(fb, 0, pw * ph * 2u);
	for (uint32_t gy = 0; gy < gh; ++gy) {
		for (uint32_t gx = 0; gx < gw; ++gx) {
			const uint8_t *c =
				frame + (gy * 2u) * CAM_H_RES + gx * 2u;
			uint32_t v = (c[0] + c[1] + c[CAM_H_RES] +
				      c[CAM_H_RES + 1]) / 4u;
			v = (v - gmin) * 255u / span;
			uint16_t px = ((v >> 3) << 11) | ((v >> 2) << 5) |
				      (v >> 3);
			fb[(y0 + gy) * pw + x0 + gx] = px;
		}
	}
	/* Bright green border so the draw region is unmistakable in webcam
	 * photos even when the stretched image is mostly noise. */
	const uint16_t border = 0x07e0;
	for (uint32_t t = 0; t < 4u; ++t) {
		for (uint32_t x = x0 - 4; x < x0 + gw + 4; ++x) {
			fb[(y0 - 4 + t) * pw + x] = border;
			fb[(y0 + gh + 3 - t) * pw + x] = border;
		}
		for (uint32_t y = y0 - 4; y < y0 + gh + 4; ++y) {
			fb[y * pw + x0 - 4 + t] = border;
			fb[y * pw + x0 + gw + 3 - t] = border;
		}
	}
	esp_cache_msync(fb, pw * ph * 2u, ESP_CACHE_MSYNC_FLAG_DIR_C2M);
	ESP_LOGI(TAG, "grey downscale 640x360 drawn at (%u,%u) on the "
		 "panel", x0, y0);
}

/* Wait for one complete frame.  With on_get_new_trans registered the
 * CSI engine free-runs: the ISR takes the next buffer from the callback
 * (its bool return is ignored), so every frame lands in our buffer and
 * trans_que is never drained -- calling esp_cam_ctlr_receive() again
 * would just sit on a full queue.  The buffer is pre-filled with 0xa5
 * and flushed to PSRAM so any byte the DMA skipped stays recognisable.
 * Two done edges are needed: the first may straddle the memset. */
static bool grab_frame(esp_cam_ctlr_handle_t cam,
		       esp_cam_ctlr_trans_t *trans, uint32_t timeout_ms)
{
	(void)cam;
	memset(trans->buffer, 0xa5, trans->buflen);
	esp_cache_msync(trans->buffer, trans->buflen,
			ESP_CACHE_MSYNC_FLAG_DIR_C2M);
	for (uint32_t edge = 0; edge < 2; ++edge) {
		s_frame_done = false;
		uint32_t waited = 0;
		while (waited < timeout_ms && !s_frame_done) {
			/* pdMS_TO_TICKS(5) is 0 at 100 Hz -> busy spin that
			 * never waits; 10 ms is one real tick. */
			vTaskDelay(pdMS_TO_TICKS(10));
			waited += 10;
		}
		if (!s_frame_done)
			return false;
	}
	return true;
}

static uint32_t count_a5(const uint8_t *f, size_t n)
{
	uint32_t c = 0;
	for (size_t i = 0; i < n; ++i)
		c += (f[i] == 0xa5u);
	return c;
}

static uint32_t frame_mean(const uint8_t *f, size_t n)
{
	uint64_t acc = 0;
	size_t cnt = 0;
	for (size_t i = 0; i < n; i += 16) {
		acc += f[i];
		++cnt;
	}
	return (uint32_t)(acc / (cnt ? cnt : 1));
}

/* Test-pattern analysis: first 32 bytes of rows 0/1/360 plus the mean
 * of 16 vertical and 16 horizontal stripes.  Deterministic content
 * proves line length, byte alignment and buffer freshness. */
static void tp_analyze(const uint8_t *f)
{
	static const uint32_t rows[] = { 0, 1, 360 };
	char hex[32 * 3 + 1];

	for (size_t r = 0; r < sizeof(rows) / sizeof(rows[0]); ++r) {
		const uint8_t *p = f + rows[r] * CAM_H_RES;
		for (uint32_t i = 0; i < 32; ++i)
			snprintf(hex + i * 3, 4, "%02x ", p[i]);
		hex[sizeof(hex) - 1] = 0;
		ESP_LOGI(TAG, "tp row %u: %s", (unsigned)rows[r], hex);
	}
	uint32_t vs[16] = { 0 }, hs[16] = { 0 };
	for (uint32_t s = 0; s < 16; ++s) {
		uint64_t acc = 0;
		uint32_t cnt = 0;
		for (uint32_t y = 0; y < CAM_V_RES; y += 4) {
			for (uint32_t x = s * (CAM_H_RES / 16);
			     x < (s + 1) * (CAM_H_RES / 16); x += 4) {
				acc += f[y * CAM_H_RES + x];
				++cnt;
			}
		}
		vs[s] = (uint32_t)(acc / (cnt ? cnt : 1));
	}
	for (uint32_t s = 0; s < 16; ++s) {
		uint64_t acc = 0;
		uint32_t cnt = 0;
		for (uint32_t y = s * (CAM_V_RES / 16);
		     y < (s + 1) * (CAM_V_RES / 16); y += 4) {
			for (uint32_t x = 0; x < CAM_H_RES; x += 4) {
				acc += f[y * CAM_H_RES + x];
				++cnt;
			}
		}
		hs[s] = (uint32_t)(acc / (cnt ? cnt : 1));
	}
	char buf[16 * 6];
	uint32_t off = 0;
	for (uint32_t i = 0; i < 16; ++i)
		off += snprintf(buf + off, sizeof(buf) - off, "%u ",
				(unsigned)vs[i]);
	ESP_LOGI(TAG, "tp vstripes: %s", buf);
	off = 0;
	for (uint32_t i = 0; i < 16; ++i)
		off += snprintf(buf + off, sizeof(buf) - off, "%u ",
				(unsigned)hs[i]);
	ESP_LOGI(TAG, "tp hstripes: %s", buf);
}

bool camera_oracle_run(uint16_t *fb, uint32_t fb_bytes)
{
	i2c_master_dev_handle_t sccb = NULL;
	esp_cam_ctlr_handle_t cam = NULL;
	isp_proc_handle_t isp = NULL;
	esp_ldo_channel_handle_t ldo = NULL;
	uint8_t *frame = NULL;
	bool ok = false;
	uint8_t idh = 0, idl = 0;
	esp_err_t err;

	ESP_LOGI(TAG, "=== CAMERA_ORACLE: SC2356 one-frame capture ===");

	/* Drive 24 MHz XCLK on GPIO36 before releasing CAM_RST.  M5's
	 * BSP uses LEDC (bsp_cam_osc_init), but LEDC cannot reach 24 MHz
	 * here: the shim's backlight owns the only low-speed timer clock
	 * (XTAL) and 40 MHz/2 < 24 MHz anyway.  The P4 has a dedicated
	 * CAM_CLK output (GPIO matrix CAM_CLK_PAD_OUT_IDX) with a
	 * fractional divider: PLL_F160M / (6 + 2/3) = 24 MHz exactly.
	 * ORACLE_SKIP_XCLK=1 builds skip this to test whether X2 feeds
	 * the sensor on its own. */
#ifndef ORACLE_SKIP_XCLK
	esp_clk_tree_enable_src(SOC_MOD_CLK_PLL_F160M, true);
	esp_rom_gpio_connect_out_signal(CAM_XCLK_GPIO, CAM_CLK_PAD_OUT_IDX,
					false, false);
	PERIPH_RCC_ATOMIC() {
		cam_ll_enable_clk(0, true);
		cam_ll_select_clk_src(0, CAM_CLK_SRC_PLL160M);
		cam_ll_set_group_clock_coeff(0, 6, 3, 2); /* 160/(6+2/3)=24 MHz */
	}
	ESP_LOGI(TAG, "XCLK 24 MHz on GPIO%d via CAM_CLK pad", CAM_XCLK_GPIO);
#else
	ESP_LOGI(TAG, "ORACLE_SKIP_XCLK: GPIO%d left un-driven",
		 CAM_XCLK_GPIO);
#endif

	/* Power the sensor: CAM_RST high is power + out of reset. */
	ESP_RETURN_ON_ERROR(tab5_cam_power_set(true), TAG, "CAM_RST high");
	vTaskDelay(pdMS_TO_TICKS(20));

	i2c_device_config_t dev_cfg = {
		.dev_addr_length = I2C_ADDR_BIT_LEN_7,
		.device_address = SC202CS_ADDR,
		.scl_speed_hz = 400000,
	};
	err = i2c_master_bus_add_device(
		(i2c_master_bus_handle_t)tab5_i2c_bus(), &dev_cfg, &sccb);
	if (err != ESP_OK || !sccb) {
		ESP_LOGE(TAG, "sccb device add failed: %s",
			 esp_err_to_name(err));
		goto out;
	}
	if (sccb_read(sccb, 0x3107, &idh) != ESP_OK ||
	    sccb_read(sccb, 0x3108, &idl) != ESP_OK) {
		ESP_LOGE(TAG, "chip-ID read failed");
		goto out;
	}
	ESP_LOGI(TAG, "sensor chip ID 0x%02x%02x (expect eb52)", idh, idl);
	if (((uint32_t)idh << 8 | idl) != SC202CS_PID) {
		ESP_LOGE(TAG, "unexpected sensor ID");
		goto out;
	}

	for (size_t i = 0;
	     i < sizeof(sensor_init) / sizeof(sensor_init[0]); ++i) {
		uint16_t reg = sensor_init[i][0];
		if (reg == 0xffffu)
			break;
		if (reg == 0xfffeu) {
			vTaskDelay(pdMS_TO_TICKS(sensor_init[i][1]));
			continue;
		}
		if (sccb_write(sccb, reg, sensor_init[i][1]) != ESP_OK) {
			ESP_LOGE(TAG, "init write 0x%04x failed", reg);
			goto out;
		}
	}
	ESP_LOGI(TAG, "init table written (%u regs)",
		 (unsigned)(sizeof(sensor_init) / sizeof(sensor_init[0])));

	/* Canonical esp_video order: CSI + ISP are armed first, the
	 * sensor starts streaming only afterwards (S_STREAM). */

	esp_ldo_channel_config_t ldo_cfg = {
		.chan_id = CAM_LDO_CHAN,
		.voltage_mv = CAM_LDO_MV,
	};
	err = esp_ldo_acquire_channel(&ldo_cfg, &ldo);
	if (err != ESP_OK) {
		ESP_LOGE(TAG, "MIPI LDO acquire failed: %s",
			 esp_err_to_name(err));
		goto out;
	}

	/* 128 B covers the P4 cache line for the DW-GDMA writer. */
	frame = heap_caps_aligned_calloc(128, 1, FRAME_BYTES,
					 MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
	if (!frame) {
		ESP_LOGE(TAG, "frame buffer alloc failed");
		goto out;
	}

	esp_cam_ctlr_trans_t trans = {
		.buffer = frame,
		.buflen = FRAME_BYTES,
	};
	esp_cam_ctlr_csi_config_t csi_cfg = {
		.ctlr_id = 0,
		.clk_src = 0, /* default (PLL_F20M) */
		.h_res = CAM_H_RES,
		.v_res = CAM_V_RES,
		.data_lane_num = CAM_LANES,
		.lane_bit_rate_mbps = CAM_LANE_MBPS,
		.input_data_color_type = CAM_CTLR_COLOR_RAW8,
		.output_data_color_type = CAM_CTLR_COLOR_RAW8,
		.queue_items = 1,
		.byte_swap_en = false,
		.bk_buffer_dis = false,
	};
	err = esp_cam_new_csi_ctlr(&csi_cfg, &cam);
	if (err != ESP_OK) {
		ESP_LOGE(TAG, "esp_cam_new_csi_ctlr: %s",
			 esp_err_to_name(err));
		goto out;
	}

	/* On the P4 the CSI host's pixel data reaches the CSI bridge
	 * through the ISP input module, even for raw pass-through: the
	 * CSI driver alone leaves isp_in_src/mipi_data_en unset, the
	 * bridge sees zero pixels, and the DW-GDMA never moves.
	 * esp_video does this for RAW8 via flags.bypass_isp — mirror it.
	 * NB: do NOT esp_isp_enable() a bypassed processor, it refuses. */
	const esp_isp_processor_cfg_t isp_cfg = {
		.clk_hz = 80 * 1000 * 1000,
		.input_data_source = ISP_INPUT_DATA_SOURCE_CSI,
		.input_data_color_type = ISP_COLOR_RAW8,
		.output_data_color_type = ISP_COLOR_RAW8,
		.has_line_start_packet = false,
		.has_line_end_packet = false,
		.h_res = CAM_H_RES,
		.v_res = CAM_V_RES,
		.bayer_order = COLOR_RAW_ELEMENT_ORDER_BGGR,
		.flags.bypass_isp = true,
	};
	err = esp_isp_new_processor(&isp_cfg, &isp);
	if (err != ESP_OK) {
		ESP_LOGE(TAG, "esp_isp_new_processor: %s",
			 esp_err_to_name(err));
		goto out;
	}
	ESP_LOGI(TAG, "ISP input module armed (CSI source, bypass)");

	esp_cam_ctlr_evt_cbs_t cbs = {
		.on_get_new_trans = cam_get_new_trans,
		.on_trans_finished = cam_trans_finished,
	};
	err = esp_cam_ctlr_register_event_callbacks(cam, &cbs, &trans);
	if (err != ESP_OK ||
	    (err = esp_cam_ctlr_enable(cam)) != ESP_OK ||
	    (err = esp_cam_ctlr_start(cam)) != ESP_OK) {
		ESP_LOGE(TAG, "csi setup/start: %s", esp_err_to_name(err));
		goto out;
	}
	/* Clear latched host interrupt status, then start the sensor
	 * stream AFTER the host is armed so it always sees a clean
	 * frame start (streaming before arm latches seq_frame_fatal). */
	MIPI_CSI_HOST.int_st_phy_fatal.val = ~0u;
	MIPI_CSI_HOST.int_st_pkt_fatal.val = ~0u;
	MIPI_CSI_HOST.int_st_bndry_frame_fatal.val = ~0u;
	MIPI_CSI_HOST.int_st_seq_frame_fatal.val = ~0u;
	MIPI_CSI_HOST.int_st_crc_frame_fatal.val = ~0u;
	MIPI_CSI_HOST.int_st_pld_crc_fatal.val = ~0u;
	MIPI_CSI_HOST.int_st_data_id.val = ~0u;
	MIPI_CSI_HOST.int_st_main.val = ~0u;
	/* The published table is init-only; streaming needs the separate
	 * 0x0100=0x01 write (sc202cs_set_stream in esp_cam_sensor). */
	if (sccb_write(sccb, 0x0100, 0x01) != ESP_OK) {
		ESP_LOGE(TAG, "stream-on write failed");
		goto out;
	}
	vTaskDelay(pdMS_TO_TICKS(50)); /* sensor ramp to the first frame */
	memset(frame, 0xa5, FRAME_BYTES); /* stale bytes stay visible */
	esp_cache_msync(frame, FRAME_BYTES, ESP_CACHE_MSYNC_FLAG_DIR_C2M);
	s_frame_done = false;
	err = esp_cam_ctlr_receive(cam, &trans, 3000);
	if (err != ESP_OK) {
		ESP_LOGE(TAG, "receive queue: %s", esp_err_to_name(err));
		goto out;
	}
	/* receive() only queues the transaction; the DMA completion is
	 * on_trans_finished (ISR) -> s_frame_done.  Watch the PHY while
	 * waiting: any HS activity on the lanes clears a stopstate bit. */
	uint32_t ever_low = 0, rx_or = 0;
	uint32_t d0_toggles = 0, clk_toggles = 0;
	uint8_t last_d0 = 1, last_ck = 1;
	for (uint32_t waited = 0; waited < 3000 && !s_frame_done;
	     waited += 10) {
		uint32_t s = MIPI_CSI_HOST.phy_stopstate.val;
		uint32_t r = MIPI_CSI_HOST.phy_rx.val;
		ever_low |= ~s;
		rx_or |= r;
		if ((s & 0x1u) != last_d0) {
			++d0_toggles;
			last_d0 = s & 0x1u;
		}
		if (((r >> 17) & 0x1u) != last_ck) {
			++clk_toggles;
			last_ck = (r >> 17) & 0x1u;
		}
		vTaskDelay(pdMS_TO_TICKS(10));
	}
	ESP_LOGI(TAG, "lane watch: stop_ever_low(l1:l0,clk)=0x%08" PRIx32
		 " rx_or=0x%08" PRIx32 " d0_tog=%" PRIu32
		 " clkhs_tog=%" PRIu32,
		 ever_low & 0x10003, rx_or, d0_toggles, clk_toggles);
	if (!s_frame_done || !trans.received_size) {
		ESP_LOGE(TAG, "frame timeout: size=%u",
			 (unsigned)trans.received_size);
		/* Did the PHY/host see anything? */
		ESP_LOGI(TAG, "host version=0x%08" PRIx32 " n_lanes=0x%08" PRIx32
			 " csi2_resetn=0x%08" PRIx32,
			 MIPI_CSI_HOST.version.val, MIPI_CSI_HOST.n_lanes.val,
			 MIPI_CSI_HOST.csi2_resetn.val);
		ESP_LOGI(TAG, "phy_rx=0x%08" PRIx32 " phy_stopstate=0x%08" PRIx32
			 " phy_cal=0x%08" PRIx32 " phy_test0=0x%08" PRIx32
			 " phy_test1=0x%08" PRIx32,
			 MIPI_CSI_HOST.phy_rx.val, MIPI_CSI_HOST.phy_stopstate.val,
			 MIPI_CSI_HOST.phy_cal.val,
			 MIPI_CSI_HOST.phy_test_ctrl0.val,
			 MIPI_CSI_HOST.phy_test_ctrl1.val);
		ESP_LOGI(TAG,
			 "brg: en=0x%08" PRIx32 " clk=0x%08" PRIx32
			 " dmareq=0x%08" PRIx32 " flow=0x%08" PRIx32
			 " dtype=0x%08" PRIx32 " fcfg=0x%08" PRIx32,
			 MIPI_CSI_BRIDGE.csi_en.val, MIPI_CSI_BRIDGE.clk_en.val,
			 MIPI_CSI_BRIDGE.dma_req_cfg.val,
			 MIPI_CSI_BRIDGE.buf_flow_ctl.val,
			 MIPI_CSI_BRIDGE.data_type_cfg.val,
			 MIPI_CSI_BRIDGE.frame_cfg.val);
		ESP_LOGI(TAG,
			 "brg: endian=0x%08" PRIx32 " iraw=0x%08" PRIx32
			 " ist=0x%08" PRIx32 " iena=0x%08" PRIx32
			 " dmainterval=0x%08" PRIx32 " dmablk=0x%08" PRIx32,
			 MIPI_CSI_BRIDGE.endian_mode.val,
			 MIPI_CSI_BRIDGE.int_raw.val, MIPI_CSI_BRIDGE.int_st.val,
			 MIPI_CSI_BRIDGE.int_ena.val,
			 MIPI_CSI_BRIDGE.dma_req_interval.val,
			 MIPI_CSI_BRIDGE.dmablk_size.val);
		ESP_LOGI(TAG, "brg: hctrl=0x%08" PRIx32,
			 MIPI_CSI_BRIDGE.host_ctrl.val);
		ESP_LOGI(TAG, "int_st_main=0x%08" PRIx32 " phy_fatal=0x%08" PRIx32
			 " pkt_fatal=0x%08" PRIx32 " data_id=0x%08" PRIx32
			 " bndry=0x%08" PRIx32 " seq=0x%08" PRIx32
			 " crc=0x%08" PRIx32 " pld_crc=0x%08" PRIx32,
			 MIPI_CSI_HOST.int_st_main.val,
			 MIPI_CSI_HOST.int_st_phy_fatal.val,
			 MIPI_CSI_HOST.int_st_pkt_fatal.val,
			 MIPI_CSI_HOST.int_st_data_id.val,
			 MIPI_CSI_HOST.int_st_bndry_frame_fatal.val,
			 MIPI_CSI_HOST.int_st_seq_frame_fatal.val,
			 MIPI_CSI_HOST.int_st_crc_frame_fatal.val,
			 MIPI_CSI_HOST.int_st_pld_crc_fatal.val);
		uint8_t stream_rb = 0;
		if (sccb_read(sccb, 0x0100, &stream_rb) == ESP_OK)
			ESP_LOGI(TAG, "sensor 0x0100 readback = 0x%02x",
				 stream_rb);
		/* Verify the table writes actually landed (writes ACK even
		 * if the sensor ignored them). */
		static const uint16_t probe[] = {
			0x3208, 0x3209, 0x320a, 0x320b, 0x3301,
			0x301f, 0x3031, 0x36e9, 0x3e01, 0x3908,
		};
		for (size_t i = 0;
		     i < sizeof(probe) / sizeof(probe[0]); ++i) {
			uint8_t v = 0;
			if (sccb_read(sccb, probe[i], &v) == ESP_OK)
				ESP_LOGI(TAG, "sensor 0x%04x = 0x%02x",
					 probe[i], v);
		}
		dump_csi_regs();
		goto out;
	}
	ESP_LOGI(TAG, "frame captured: %u bytes into %p",
		 (unsigned)trans.received_size, frame);

	/* Reference register state while the controller is live. */
	dump_csi_regs();

	frame_stats(frame, trans.received_size);
	ESP_LOGI(TAG, "cap live-default: a5_left=%u",
		 count_a5(frame, FRAME_BYTES));

	/* ---- (a) test pattern: sensor 0x4501 bit3 emits a
	 * deterministic ramp/bars; byte-level content proves line
	 * length, alignment and buffer freshness. */
	{
		uint8_t tp = 0;
		if (sccb_read(sccb, 0x4501, &tp) == ESP_OK &&
		    sccb_write(sccb, 0x4501, tp | 0x08u) == ESP_OK) {
			(void)grab_frame(cam, &trans, 1500); /* take effect */
			if (grab_frame(cam, &trans, 2000)) {
				ESP_LOGI(TAG, "cap test-pattern: a5_left=%u",
					 count_a5(frame, FRAME_BYTES));
				tp_analyze(frame);
			} else {
				ESP_LOGE(TAG, "test-pattern frame timeout");
			}
			sccb_write(sccb, 0x4501, tp); /* restore */
		} else {
			ESP_LOGE(TAG, "test-pattern 0x4501 write failed");
		}
	}

	/* ---- (b) light response: 1x vs 32x total gain at the same
	 * exposure; mean-16 must scale.  0x3e06=dig coarse,
	 * 0x3e07=dig fine, 0x3e09=ana coarse (driver gain map). */
	static const struct {
		const char *name;
		uint8_t fine, coarse, ana;
	} gains[] = {
		{ "gain-1x", 0x80, 0x00, 0x00 },
		{ "gain-32x", 0xfc, 0x01, 0x0f },
	};
	for (size_t gi = 0; gi < sizeof(gains) / sizeof(gains[0]); ++gi) {
		sccb_write(sccb, 0x3e07, gains[gi].fine);
		sccb_write(sccb, 0x3e06, gains[gi].coarse);
		sccb_write(sccb, 0x3e09, gains[gi].ana);
		(void)grab_frame(cam, &trans, 1500); /* settle */
		(void)grab_frame(cam, &trans, 1500); /* settle */
		if (grab_frame(cam, &trans, 2000)) {
			uint32_t m = frame_mean(frame, FRAME_BYTES);
			ESP_LOGI(TAG, "cap %s: mean=%u mean-16=%d a5_left=%u",
				 gains[gi].name, (unsigned)m,
				 (int)m - 16, count_a5(frame, FRAME_BYTES));
		} else {
			ESP_LOGE(TAG, "%s frame timeout", gains[gi].name);
		}
	}

	/* ---- (c) brightest real frame for the panel: longest
	 * exposure the mode allows (VTS-6) at 32x gain.  VTS lives
	 * in 0x320e/0x320f; shutter is 0x3e00-0x3e02 in 1/16-line
	 * units (sc202cs_set_exp_val). */
	{
		uint8_t vh = 0, vl = 0;
		uint32_t vts = 0, exp = 0x3dc;
		if (sccb_read(sccb, 0x320e, &vh) == ESP_OK &&
		    sccb_read(sccb, 0x320f, &vl) == ESP_OK)
			vts = ((uint32_t)vh << 8) | vl;
		if (vts > 8u)
			exp = vts - 6u;
		sccb_write(sccb, 0x3e00, (exp >> 12) & 0x0fu);
		sccb_write(sccb, 0x3e01, (exp >> 4) & 0xffu);
		sccb_write(sccb, 0x3e02, (exp & 0x0fu) << 4);
		ESP_LOGI(TAG, "vts=%u shutter=%u (1/16-line units)",
			 (unsigned)vts, (unsigned)exp);
		(void)grab_frame(cam, &trans, 1500); /* settle */
		(void)grab_frame(cam, &trans, 1500); /* settle */
		if (grab_frame(cam, &trans, 2000)) {
			uint32_t m = frame_mean(frame, FRAME_BYTES);
			ESP_LOGI(TAG, "cap max-exp+32x: mean=%u mean-16=%d "
				 "a5_left=%u", (unsigned)m, (int)m - 16,
				 count_a5(frame, FRAME_BYTES));
			frame_stats(frame, trans.received_size);
		} else {
			ESP_LOGE(TAG, "max-exp frame timeout");
		}
	}

	if (fb)
		draw_on_panel(fb, fb_bytes, frame);
	ESP_LOGI(TAG, "ORACLE HOLD START: frame on panel for %u ms",
		 (unsigned)HOLD_ON_PANEL_MS);
	vTaskDelay(pdMS_TO_TICKS(HOLD_ON_PANEL_MS));
	ok = true;

out:
	/* Quiesce the whole pipe before the Linux jump: a live DW-GDMA
	 * channel keeps writing into the (now Linux-owned) PSRAM buffer
	 * and panics the kernel during early boot.  Order matters:
	 * stop -> disable -> del is the only sequence the state machine
	 * accepts; del releases the GDMA channel.  Also stop the sensor
	 * stream so nothing new enters the CSI host. */
	if (sccb)
		sccb_write(sccb, 0x0100, 0x00); /* sensor stream off */
	if (cam) {
		esp_cam_ctlr_stop(cam);
		esp_cam_ctlr_disable(cam);
		esp_cam_ctlr_del(cam);
	}
	if (isp)
		esp_isp_del_processor(isp);
	/* Belt and braces: hard-disarm the bridge + GDMA channels so no
	 * residual engine can move data even if driver teardown raced. */
	MIPI_CSI_BRIDGE.csi_en.csi_brg_en = 0;
	DW_GDMA.chen0.val = 0;
	if (ldo)
		esp_ldo_release_channel(ldo);
	if (sccb)
		i2c_master_bus_rm_device(sccb);
	heap_caps_free(frame);
	/* Leave CAM_RST high across the Linux jump? No -- the rail policy
	 * keeps the camera off; the Linux driver will own P6. */
	tab5_cam_power_set(false);
	PERIPH_RCC_ATOMIC() {
		cam_ll_enable_clk(0, false);
	}
	ESP_LOGI(TAG, "=== CAMERA_ORACLE done: %s ===",
		 ok ? "CAPTURE OK" : "FAILED");
	return ok;
}
