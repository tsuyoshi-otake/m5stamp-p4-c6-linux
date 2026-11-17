// SPDX-License-Identifier: Apache-2.0 OR GPL-2.0-only
/*
 * AUDIO_ORACLE (#24): capture the known-good I2S0 + AHB-GDMA register
 * state while IDF's own i2s_std driver is actually playing a 1 kHz tone
 * through the ES8388, so the Linux driver can be diffed against a
 * working reference on this exact board.
 *
 * Wired in after tab5_board_init() (the internal I2C bus is up, the
 * codec at 0x10 and the speaker-amp latch on expander 0x43 P1 are
 * reachable) and before tab5_i2c_release() hands the bus back to plain
 * GPIOs.  The codec + I2S channel are torn down again afterwards so the
 * Linux hand-off state is unchanged apart from the brief tone.
 */

#include <math.h>
#include <stdint.h>
#include <string.h>
#include <inttypes.h>

#include "esp_err.h"
#include "esp_log.h"
#include "driver/i2c_master.h"
#include "driver/i2s_std.h"
#include "driver/i2s_tdm.h"
#include "driver/i2s_types.h"
#include "tab5_board.h"
#include "audio_oracle.h"

static const char *TAG = "audio-oracle";

#define ES8388_ADDR 0x10u

#define I2S0_BASE 0x500c6000u
#define AHB_GDMA_BASE 0x50085000u
#define HP_SYS_CLKRST_BASE 0x500e6000u
#define LP_CLKRST_BASE 0x50111000u
#define GPIO_BASE 0x500e0000u
#define GPIO_FUNC_IN_SEL_BASE (GPIO_BASE + 0x158u)
#define GPIO_FUNC_OUT_SEL_BASE (GPIO_BASE + 0x558u)
#define IO_MUX_BASE 0x500e1000u

/* Tab5 I2S pair to the ES8388 (linux/dts/esp32p4-tab5.dts i2s0 node). */
#define PIN_MCLK 30
#define PIN_BCK 27
#define PIN_WS 29
#define PIN_DOUT 26
#define PIN_DIN 28

#define SAMPLE_RATE 48000u
#define TONE_HZ 1000u
#define SINE_AMPLITUDE 12000
#define FRAMES_PER_PERIOD (SAMPLE_RATE / TONE_HZ) /* 48 */
#define CHUNK_PERIODS 10 /* 480 frames = 10 ms per write */
#define CHUNK_FRAMES (FRAMES_PER_PERIOD * CHUNK_PERIODS)
#define WRITE_CHUNKS 200 /* ~= 2.0 s of tone */
#define DUMP_AFTER_CHUNKS 60 /* dump mid-play, ~0.6 s in */

static inline volatile uint32_t *reg32(uint32_t addr)
{
	return (volatile uint32_t *)(uintptr_t)addr;
}

static void dump_range(const char *name, uint32_t base, uint32_t first_off,
		       uint32_t last_off)
{
	uint32_t off;

	for (off = first_off; off <= last_off; off += 4u)
		ESP_LOGI(TAG, "R %s 0x%08" PRIx32 " = 0x%08" PRIx32, name,
			 base + off, *reg32(base + off));
}

static void dump_list(const char *name, uint32_t base, const uint32_t *offs,
		      uint32_t n)
{
	for (uint32_t i = 0; i < n; ++i)
		ESP_LOGI(TAG, "R %s 0x%08" PRIx32 " = 0x%08" PRIx32, name,
			 base + offs[i], *reg32(base + offs[i]));
}

/* Descriptor live pointers in a TX/RX channel window (offsets match the
 * Linux driver's AHB_CH_* layout). */
static void dump_channel_window(uint32_t win_base, const char *name,
				uint32_t index)
{
	uint32_t addrs[4];
	const char *which[4] = { "DSCR", "DSCR_BF0", "DSCR_BF1", "EOF_DES" };
	uint32_t i, w;

	dump_range(name, win_base, 0x00, 0x30);
	addrs[0] = *reg32(win_base + 0x20u); /* AHB_DSCR */
	addrs[1] = *reg32(win_base + 0x24u); /* AHB_DSCR_BF0 */
	addrs[2] = *reg32(win_base + 0x28u); /* AHB_DSCR_BF1 */
	addrs[3] = *reg32(win_base + 0x18u); /* *_EOF_DES */
	for (i = 0; i < 4u; ++i) {
		if (!addrs[i])
			continue;
		for (w = 0; w < 3u; ++w)
			ESP_LOGI(TAG, "D %s%" PRIu32 " %s 0x%08" PRIx32
				 " = 0x%08" PRIx32, name, index, which[i],
				 addrs[i] + w * 4u,
				 *reg32(addrs[i] + w * 4u));
	}
}

static void dump_regs(void)
{
	static const uint32_t clkrst_offs[] = { 0x1c, 0x44, 0x48, 0x4c, 0x50,
						0xc8 };
	static const uint32_t lpclkrst_offs[] = { 0x40 };
	/* OUT_SEL for MCLK/BCK/WS/DOUT pins; IN_SEL for the five I2S0
	 * input signal indexes (I2S0_O_BCK/O_WS are shared in/out). */
	static const uint32_t outsel_pins[] = { PIN_MCLK, PIN_BCK, PIN_WS,
						PIN_DOUT };
	static const uint32_t insel_sigs[] = { 25, 26, 27, 28, 29, 30 };
	static const uint32_t iomux_pins[] = { PIN_DOUT, PIN_BCK, PIN_DIN,
						PIN_WS, PIN_MCLK };
	uint32_t ch, i;

	ESP_LOGI(TAG, "=== AUDIO_ORACLE dump (I2S active) ===");
	dump_range("i2s", I2S0_BASE, 0x00, 0x7c);
	dump_range("ahd", AHB_GDMA_BASE, 0x00, 0x64);
	for (ch = 0u; ch < 3u; ++ch) {
		dump_channel_window(AHB_GDMA_BASE + 0x70u + ch * 0xc0u, "in",
				    ch);
		dump_channel_window(AHB_GDMA_BASE + 0xd0u + ch * 0xc0u, "out",
				    ch);
	}
	dump_range("inlnk", AHB_GDMA_BASE + 0x3acu, 0x00, 0x08);
	dump_range("outlnk", AHB_GDMA_BASE + 0x3b8u, 0x00, 0x08);
	dump_list("clkrst", HP_SYS_CLKRST_BASE, clkrst_offs,
		  sizeof(clkrst_offs) / sizeof(clkrst_offs[0]));
	dump_list("lpclk", LP_CLKRST_BASE, lpclkrst_offs,
		  sizeof(lpclkrst_offs) / sizeof(lpclkrst_offs[0]));
	for (i = 0u; i < sizeof(outsel_pins) / sizeof(outsel_pins[0]); ++i)
		ESP_LOGI(TAG, "R outsel%u 0x%08" PRIx32 " = 0x%08" PRIx32,
			 outsel_pins[i],
			 GPIO_FUNC_OUT_SEL_BASE + outsel_pins[i] * 4u,
			 *reg32(GPIO_FUNC_OUT_SEL_BASE + outsel_pins[i] * 4u));
	for (i = 0u; i < sizeof(insel_sigs) / sizeof(insel_sigs[0]); ++i)
		ESP_LOGI(TAG, "R insel%" PRIu32 " 0x%08" PRIx32
			 " = 0x%08" PRIx32, insel_sigs[i],
			 GPIO_FUNC_IN_SEL_BASE + insel_sigs[i] * 4u,
			 *reg32(GPIO_FUNC_IN_SEL_BASE + insel_sigs[i] * 4u));
	for (i = 0u; i < sizeof(iomux_pins) / sizeof(iomux_pins[0]); ++i)
		ESP_LOGI(TAG, "R iomux%u 0x%08" PRIx32 " = 0x%08" PRIx32,
			 iomux_pins[i],
			 IO_MUX_BASE + (iomux_pins[i] + 1u) * 4u,
			 *reg32(IO_MUX_BASE + (iomux_pins[i] + 1u) * 4u));
	ESP_LOGI(TAG, "=== AUDIO_ORACLE dump end ===");
}

/*
 * Minimal ES8388 DAC playback init: the register sequence esp_codec_dev
 * writes in es8388_open() for mode=DAC plus the es8388_start() unmute
 * (components/esp_codec_dev/device/es8388/es8388.c in esp-adf), minus the
 * ADC power-on -- only the speaker path is exercised.
 */
static const uint8_t es8388_dac_init[][2] = {
	{ 0x19, 0x04 }, /* DACCONTROL3: mute with ramp */
	{ 0x01, 0x50 }, /* CONTROL2 */
	{ 0x02, 0x00 }, /* CHIPPOWER: power up all */
	{ 0x35, 0xa0 }, /* DLL off (low-rate improvement) */
	{ 0x37, 0xd0 },
	{ 0x39, 0xd0 },
	{ 0x08, 0x00 }, /* MASTERMODE: codec is I2S slave */
	{ 0x04, 0xc0 }, /* DACPOWER: DAC + LOUT/ROUT off for now */
	{ 0x00, 0x12 }, /* CONTROL1: play mode */
	{ 0x17, 0x18 }, /* DACCONTROL1: 16-bit I2S data format */
	{ 0x18, 0x02 }, /* DACCONTROL2: single speed, MCLK/LRCK=256 */
	{ 0x26, 0x00 }, /* DACCONTROL16: LIN1/RIN1 */
	{ 0x27, 0x90 }, /* DACCONTROL17: left DAC -> left mixer */
	{ 0x2a, 0x90 }, /* DACCONTROL20: right DAC -> right mixer */
	{ 0x2b, 0x80 }, /* DACCONTROL21: dac on, same LRCK */
	{ 0x2d, 0x00 }, /* DACCONTROL23: vroi=0 */
	{ 0x1a, 0x00 }, /* DACCONTROL4: L DAC volume 0 dB */
	{ 0x1b, 0x00 }, /* DACCONTROL5: R DAC volume 0 dB */
	{ 0x2e, 0x1e }, /* DACCONTROL24: L1 mixer vol 0 dB */
	{ 0x2f, 0x1e }, /* DACCONTROL25: R1 mixer vol 0 dB */
	{ 0x30, 0x00 },
	{ 0x31, 0x00 },
	{ 0x04, 0x3c }, /* DACPOWER: DAC + LOUT1/2 + ROUT1/2 on */
	{ 0x03, 0xff }, /* ADCPOWER: ADC stays off */
	{ 0x19, 0x00 }, /* DACCONTROL3: unmute */
};

static esp_err_t es8388_init_playback(i2c_master_bus_handle_t bus,
				      i2c_master_dev_handle_t *out_dev)
{
	i2c_device_config_t dev_cfg = {
		.dev_addr_length = I2C_ADDR_BIT_LEN_7,
		.device_address = ES8388_ADDR,
		.scl_speed_hz = 400000u,
	};
	i2c_master_dev_handle_t dev = NULL;
	esp_err_t err;
	uint32_t i;

	err = i2c_master_bus_add_device(bus, &dev_cfg, &dev);
	if (err != ESP_OK)
		return err;
	for (i = 0u; i < sizeof(es8388_dac_init) / sizeof(es8388_dac_init[0]);
	     ++i) {
		uint8_t buf[2] = { es8388_dac_init[i][0],
				   es8388_dac_init[i][1] };
		err = i2c_master_transmit(dev, buf, sizeof(buf), 100);
		if (err != ESP_OK)
			break;
	}
	if (err != ESP_OK) {
		i2c_master_bus_rm_device(dev);
		return err;
	}
	*out_dev = dev;
	ESP_LOGI(TAG, "es8388 init: %" PRIu32 " regs written at 0x%02x", i,
		 ES8388_ADDR);
	return ESP_OK;
}

static int16_t sine[CHUNK_FRAMES * 2]; /* stereo frames, L=R */

/* One 480-frame chunk = 10 periods of 1 kHz at 48 kHz stereo. */
static void fill_sine(void)
{
	uint32_t i;
	for (i = 0u; i < CHUNK_FRAMES; ++i) {
		float s = sinf(2.0f * 3.14159265358979f *
			       (float)(i % FRAMES_PER_PERIOD) /
			       (float)FRAMES_PER_PERIOD);
		int16_t v = (int16_t)(s * (float)SINE_AMPLITUDE);
		sine[i * 2u] = v;
		sine[i * 2u + 1u] = v;
	}
}

bool audio_oracle_run(void)
{
	i2c_master_bus_handle_t bus;
	i2c_master_dev_handle_t codec = NULL;
	i2s_chan_handle_t tx = NULL;
	esp_err_t err;
	size_t written = 0;
	uint32_t total_frames = 0;
	uint32_t chunk, i;
	bool ok = false;

	bus = tab5_i2c_bus();
	if (!bus) {
		ESP_LOGE(TAG, "internal i2c bus is not up; oracle skipped");
		return false;
	}

	fill_sine();

	if (es8388_init_playback(bus, &codec) != ESP_OK) {
		ESP_LOGE(TAG, "es8388 init failed; oracle skipped");
		return false;
	}
	if (tab5_speaker_amp_set(true) != ESP_OK)
		ESP_LOGW(TAG, "speaker amp enable failed; tone may be inaudible");

	{
		i2s_chan_config_t chan_cfg =
			I2S_CHANNEL_DEFAULT_CONFIG(I2S_NUM_0, I2S_ROLE_MASTER);
		i2s_std_config_t std_cfg = {
			.clk_cfg = I2S_STD_CLK_DEFAULT_CONFIG(SAMPLE_RATE),
			.slot_cfg = I2S_STD_PHILIPS_SLOT_DEFAULT_CONFIG(
				I2S_DATA_BIT_WIDTH_16BIT, I2S_SLOT_MODE_STEREO),
			.gpio_cfg = {
				.mclk = PIN_MCLK,
				.bclk = PIN_BCK,
				.ws = PIN_WS,
				.dout = PIN_DOUT,
				.din = PIN_DIN,
				.invert_flags = {
					.mclk_inv = false,
					.bclk_inv = false,
					.ws_inv = false,
				},
			},
		};

		chan_cfg.dma_desc_num = 6;
		chan_cfg.dma_frame_num = 240;

		err = i2s_new_channel(&chan_cfg, &tx, NULL);
		if (err == ESP_OK)
			err = i2s_channel_init_std_mode(tx, &std_cfg);
		if (err == ESP_OK)
			err = i2s_channel_enable(tx);
		if (err != ESP_OK) {
			ESP_LOGE(TAG, "i2s channel setup failed: %s",
				 esp_err_to_name(err));
			if (tx)
				i2s_del_channel(tx);
			goto out;
		}
	}

	for (chunk = 0u; chunk < WRITE_CHUNKS; ++chunk) {
		err = i2s_channel_write(tx, sine, sizeof(sine), &written, 1000);
		if (err != ESP_OK) {
			ESP_LOGE(TAG, "i2s write %s at chunk %" PRIu32,
				 esp_err_to_name(err), chunk);
			break;
		}
		total_frames += written / 4u;
		if (chunk == DUMP_AFTER_CHUNKS)
			dump_regs();
	}
	ESP_LOGI(TAG, "i2s wrote %" PRIu32 " frames (%" PRIu32 " bytes)",
		 total_frames, total_frames * 4u);
	dump_regs(); /* post-write state: PARK / EOF / descriptor heads */

	i2s_channel_disable(tx);
	i2s_del_channel(tx);
	ok = true;

out:
	(void)tab5_speaker_amp_set(false);
	(void)i2c_master_bus_rm_device(codec);
	ESP_LOGI(TAG, "AUDIO_ORACLE %s", ok ? "done" : "FAILED");
	return ok;
}

/* ------------------------------------------------------------------ */
/* ES7210 microphone capture reference (#24).  Same transcription rule
 * as the ES8388 table above: this is esp_codec_dev's es7210_open() +
 * es7210_set_fs() + es7210_enable(true) register flow from
 * device/es7210/es7210.c in esp-adf, run over the BSP's exact channel
 * layout from M5Tab5-UserDemo's m5stack_tab5.c (TX std Philips 16-bit
 * mono, RX TDM 4-slot stereo 16-bit @ 48 kHz, mclk_multiple 256,
 * bclk_div 8, all four MIC inputs selected, fs = 48 kHz/4ch/16bit). */
#define ES7210_ADDR 0x40u
#define MIC_CHUNK_BYTES 8192u
#define MIC_CHUNKS 12u /* ~1 s of 48 kHz stereo 16-bit */

static i2c_master_dev_handle_t s_adc;

static esp_err_t es7210_w(uint8_t reg, uint8_t val)
{
	uint8_t buf[2] = { reg, val };
	return i2c_master_transmit(s_adc, buf, sizeof(buf), 100);
}

static esp_err_t es7210_r(uint8_t reg, uint8_t *val)
{
	return i2c_master_transmit_receive(s_adc, &reg, 1, val, 1, 100);
}

static esp_err_t es7210_u(uint8_t reg, uint8_t mask, uint8_t data)
{
	uint8_t v = 0;
	esp_err_t e = es7210_r(reg, &v);
	if (e != ESP_OK)
		return e;
	return es7210_w(reg, (uint8_t)((v & ~mask) | (mask & data)));
}

/* es7210_select_mics(0x0F): enable all four ADC front-ends like the BSP. */
static void es7210_select_mics_all(void)
{
	uint8_t i;
	for (i = 0; i < 4u; ++i)
		es7210_u((uint8_t)(0x43u + i), 0x10, 0x00);
	es7210_w(0x4b, 0xff);
	es7210_w(0x4c, 0xff);
	es7210_u(0x01, 0x0b, 0x00);
	es7210_w(0x4b, 0x00);
	es7210_u(0x43, 0x10, 0x10); /* MIC1 */
	es7210_u(0x01, 0x0b, 0x00);
	es7210_w(0x4b, 0x00);
	es7210_u(0x44, 0x10, 0x10); /* MIC2 */
	es7210_u(0x01, 0x15, 0x00);
	es7210_w(0x4c, 0x00);
	es7210_u(0x45, 0x10, 0x10); /* MIC3 */
	es7210_u(0x01, 0x15, 0x00);
	es7210_w(0x4c, 0x00);
	es7210_u(0x46, 0x10, 0x10); /* MIC4 */
}

static void dump_es7210_regs(void)
{
	uint8_t reg, v;
	ESP_LOGI(TAG, "=== ES7210 register dump ===");
	for (reg = 0x00; reg <= 0x4f; ++reg) {
		v = 0;
		if (es7210_r(reg, &v) == ESP_OK)
			ESP_LOGI(TAG, "ES7210 0x%02x = 0x%02x", reg, v);
		else
			ESP_LOGW(TAG, "ES7210 0x%02x read failed", reg);
	}
	ESP_LOGI(TAG, "=== ES7210 dump end ===");
}

bool audio_oracle_mic_run(void)
{
	static int16_t micbuf[MIC_CHUNK_BYTES / 2];
	i2c_master_bus_handle_t bus;
	i2s_chan_handle_t tx = NULL, rx = NULL;
	uint8_t off_reg = 0;
	esp_err_t err;
	size_t got;
	uint64_t sumsq = 0;
	int32_t peak = 0, nonzero = 0, samples = 0;
	uint32_t i;
	uint8_t i2;
	bool ok = false;

	bus = tab5_i2c_bus();
	if (!bus) {
		ESP_LOGE(TAG, "internal i2c bus is not up; mic oracle skipped");
		return false;
	}

	/* es7210_open() register flow. */
	{
		i2c_device_config_t dev_cfg = {
			.dev_addr_length = I2C_ADDR_BIT_LEN_7,
			.device_address = ES7210_ADDR,
			.scl_speed_hz = 400000u,
		};
		err = i2c_master_bus_add_device(bus, &dev_cfg, &s_adc);
		if (err != ESP_OK) {
			ESP_LOGE(TAG, "es7210 i2c attach failed: %s",
				 esp_err_to_name(err));
			return false;
		}
	}
	es7210_w(0x00, 0xff);
	es7210_w(0x00, 0x41);
	es7210_w(0x01, 0x3f);
	es7210_w(0x09, 0x30);
	es7210_w(0x0a, 0x30);
	es7210_w(0x23, 0x2a);
	es7210_w(0x22, 0x0a);
	es7210_w(0x20, 0x0a);
	es7210_w(0x21, 0x2a);
	es7210_u(0x08, 0x01, 0x00); /* slave mode */
	es7210_w(0x40, 0x43);
	es7210_w(0x41, 0x70);
	es7210_w(0x42, 0x70);
	es7210_w(0x07, 0x20);
	es7210_w(0x02, 0xc1);
	es7210_select_mics_all();
	for (i2 = 0; i2 < 4u; ++i2) /* set_channel_gain(0x0F, 30 dB) */
		es7210_u((uint8_t)(0x43u + i2), 0x0f, 0x0a);
	es7210_w(0x12, 0x00); /* non-TDM stereo (matches std-RX frame) */
	es7210_r(0x01, &off_reg);
	ESP_LOGI(TAG, "es7210 open done; off_reg=0x%02x", off_reg);

	/* es7210_set_fs(48000, 4ch, 16bit): bits=16 -> 0x11 |= 0x60,
	 * fmt NORMAL -> 0x11 &= ~0x03.  Slave: rate table skipped. */
	es7210_u(0x11, 0xe3, 0x60);

	/* BSP channel layout: one channel, TX std mono + RX TDM4. */
	{
		i2s_chan_config_t chan_cfg =
			I2S_CHANNEL_DEFAULT_CONFIG(I2S_NUM_0, I2S_ROLE_MASTER);
		chan_cfg.auto_clear = true;
		err = i2s_new_channel(&chan_cfg, &tx, &rx);
		if (err != ESP_OK) {
			ESP_LOGE(TAG, "i2s_new_channel: %s",
				 esp_err_to_name(err));
			goto out;
		}
	}
	{
		i2s_std_config_t std_cfg = {
			.clk_cfg = I2S_STD_CLK_DEFAULT_CONFIG(48000),
			.slot_cfg = {
				.data_bit_width = I2S_DATA_BIT_WIDTH_16BIT,
				.slot_bit_width = I2S_SLOT_BIT_WIDTH_AUTO,
				.slot_mode = I2S_SLOT_MODE_STEREO,
				.slot_mask = I2S_STD_SLOT_BOTH,
				.ws_width = I2S_DATA_BIT_WIDTH_16BIT,
				.ws_pol = false,
				.bit_shift = true,
				.left_align = true,
				.big_endian = false,
				.bit_order_lsb = false,
			},
			.gpio_cfg = {
				.mclk = PIN_MCLK,
				.bclk = PIN_BCK,
				.ws = PIN_WS,
				.dout = PIN_DOUT,
				.din = PIN_DIN,
				.invert_flags = {
					.mclk_inv = false,
					.bclk_inv = false,
					.ws_inv = false,
				},
			},
		};
		/* Experiment: std stereo RX shares TX's 32-BCK frame
		 * exactly (TDM4 RX never completed a frame on the
		 * 32-BCK WS cadence -> read timeout). */
		i2s_std_config_t rx_std = std_cfg;
		rx_std.gpio_cfg.dout = GPIO_NUM_NC;
		rx_std.gpio_cfg.din = PIN_DIN;
		err = i2s_channel_init_std_mode(tx, &std_cfg);
		if (err == ESP_OK)
			err = i2s_channel_init_std_mode(rx, &rx_std);
		if (err == ESP_OK) {
			/* TX has TX_STOP_EN: an underrun kills the shared
			 * BCK/WS and starves RX.  Preload the sine backlog
			 * before TX starts so it never runs dry. */
			size_t loaded = 0;
			fill_sine();
			err = i2s_channel_preload_data(tx, sine, sizeof(sine),
						       &loaded);
			ESP_LOGI(TAG, "tx preload: %u bytes", (unsigned)loaded);
		}
		if (err == ESP_OK)
			err = i2s_channel_enable(tx);
		if (err == ESP_OK)
			err = i2s_channel_enable(rx);
		if (err != ESP_OK) {
			ESP_LOGE(TAG, "i2s duplex setup failed: %s",
				 esp_err_to_name(err));
			goto out;
		}
	}

	/* es7210_enable(true): start(off_reg) then unmute all four. */
	es7210_w(0x01, off_reg);
	es7210_w(0x06, 0x00);
	es7210_w(0x40, 0x43);
	es7210_w(0x47, 0x08);
	es7210_w(0x48, 0x08);
	es7210_w(0x49, 0x08);
	es7210_w(0x4a, 0x08);
	es7210_select_mics_all();
	es7210_w(0x40, 0x43);
	es7210_w(0x00, 0x71);
	es7210_w(0x00, 0x41);
	es7210_u(0x15, 0x03, 0x00); /* ADC12 unmute */
	es7210_u(0x14, 0x03, 0x00); /* ADC34 unmute */

	/* Full-duplex RX shares TX's BCK/WS (sig_loopback).  Keep TX
	 * fed so its clock generator never underruns while recording
	 * -- this doubles as the mic loopback. */
	if (tab5_speaker_amp_set(true) != ESP_OK)
		ESP_LOGW(TAG, "speaker amp enable failed; loopback quiet");
	for (i = 0; i < MIC_CHUNKS; ++i) {
		uint32_t n, w;
		for (w = 0; w < 6u; ++w) { /* ~60 ms TX per ~43 ms read */
			size_t wrote = 0;
			(void)i2s_channel_write(tx, sine, sizeof(sine),
						&wrote, 200);
		}
		err = i2s_channel_read(rx, micbuf, sizeof(micbuf), &got, 1000);
		if (err != ESP_OK) {
			ESP_LOGE(TAG, "i2s read chunk %" PRIu32 ": %s", i,
				 esp_err_to_name(err));
			break;
		}
		for (n = 0; n < got / 2u; ++n) {
			int32_t v = micbuf[n];
			if (v != 0)
				nonzero++;
			if (v < 0)
				v = -v;
			if (v > peak)
				peak = v;
			sumsq += (uint64_t)micbuf[n] * (uint64_t)micbuf[n];
		}
		samples += (int32_t)(got / 2u);
		if (i == 0u)
			ESP_LOGI(TAG, "first samples: %d %d %d %d %d %d %d %d",
				 micbuf[0], micbuf[1], micbuf[2], micbuf[3],
				 micbuf[4], micbuf[5], micbuf[6], micbuf[7]);
	}
	ESP_LOGI(TAG,
		 "MIC capture: %" PRIi32 " samples, %" PRIi32 " nonzero, peak %" PRIi32
		 ", rms2=%llu",
		 samples, nonzero, peak,
		 samples ? (unsigned long long)(sumsq / (uint64_t)samples) : 0ull);
	dump_regs();
	dump_es7210_regs();
	ok = (nonzero > 0);

	(void)tab5_speaker_amp_set(false);
	i2s_channel_disable(tx);
	i2s_channel_disable(rx);
	i2s_del_channel(tx);
	i2s_del_channel(rx);
out:
	(void)i2c_master_bus_rm_device(s_adc);
	ESP_LOGI(TAG, "AUDIO_ORACLE mic %s", ok ? "done (nonzero)" : "FAILED (all-zero)");
	return ok;
}
