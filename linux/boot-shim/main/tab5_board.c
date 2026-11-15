// SPDX-License-Identifier: Apache-2.0 OR GPL-2.0-only
/*
 * M5Stack Tab5 board bootstrap for the Linux boot shim (BOOT_PROFILE=tab5).
 *
 * Replaces the Stamp-AddOn path (m2_sdmmc.c): Tab5 wires the on-board
 * ESP32-C6 to SDMMC slot 1 through the GPIO matrix on
 *   CLK=GPIO12 CMD=GPIO13 DAT0..3=GPIO{11,10,9,8}
 * and C6 reset/EN to GPIO15 (bank 0).  C6 power comes from IO-expander
 * 0x44 P0 (WLAN_PWR_EN), not from an always-on rail, so the expander must
 * be programmed before the reset pulse.
 *
 * Safety contract (board-contract.json): the speaker amp (0x43 P1),
 * camera (P6) and EXT5V (0x43 P2) stay OFF at boot, while USB-A 5 V
 * (0x44 P3), QC and CHG_EN follow the stock image.  The stock image
 * repeatedly tripped the brownout detector on plain USB power with
 * speaker + backlight + C6 all up (live-log.txt BOD resets), so Linux
 * keeps the conservative subset.  The LCD reset line (0x43 P4) goes
 * through the stock assert-then-release sequence (output low 10 ms,
 * then input with internal pull-up) and TP_RST (0x43 P5) is driven 1
 * as in stock.  The ST7121/ST7123 are combined display+touch chips, so
 * right before the display init the stock BSP pulses both resets low for
 * 100 ms (bsp_reset_tp); tab5_panel_reset() repeats that.
 * The backlight (GPIO22) is driven low until Linux or a user asks.
 */

#include <stdint.h>
#include <stdbool.h>
#include <string.h>
#include <inttypes.h>

#include "esp_check.h"
#include "esp_err.h"
#include "esp_log.h"
#include "esp_private/gpio.h"
#include "esp_rom_gpio.h"
#include "esp_rom_sys.h"
#include "riscv/csr.h"
#include "riscv/rv_utils.h"
#include "driver/i2c_master.h"
#include "driver/gpio.h"
#include "soc/clic_reg.h"
#include "soc/gpio_sig_map.h"
#include "soc/soc.h"
#include "soc/interrupt_core0_reg.h"
#include "tab5_board.h"
#include "tab5_display.h"

static const char *TAG = "tab5-board";

#define HP_SYS_CLKRST_BASE 0x500e6000u
#define HP_SOC_CLK_CTRL1 (HP_SYS_CLKRST_BASE + 0x18u)
#define HP_PERI_CLK_CTRL01 (HP_SYS_CLKRST_BASE + 0x34u)
#define HP_PERI_CLK_CTRL02 (HP_SYS_CLKRST_BASE + 0x38u)
#define LP_CLKRST_BASE 0x50111000u
#define LP_SDMMC_RESET (LP_CLKRST_BASE + 0x4cu)

#define GPIO_BASE 0x500e0000u
#define GPIO_OUT_W1TS (GPIO_BASE + 0x08u)
#define GPIO_OUT_W1TC (GPIO_BASE + 0x0cu)
#define GPIO_ENABLE_W1TS (GPIO_BASE + 0x24u) /* bank 0; bank 1 is +0x30 as in m2_sdmmc.c */
#define GPIO_FUNC_IN_SEL_BASE (GPIO_BASE + 0x158u)
#define GPIO_FUNC_OUT_SEL_BASE (GPIO_BASE + 0x558u)
#define IO_MUX_BASE 0x500e1000u

#define SDMMC_SYS_CLK_EN (1u << 14)
#define SDIO_HS_MODE (1u << 22)
#define SDIO_LS_CLK_SRC_SEL (1u << 23)
#define SDIO_LS_CLK_EN (1u << 24)
#define SDIO_EDGE_CFG_UPDATE (1u << 8)
#define SDIO_EDGE_L_SHIFT 9
#define SDIO_EDGE_H_SHIFT 13
#define SDIO_EDGE_N_SHIFT 17
#define SDIO_SLF_EDGE_SEL_SHIFT 21
#define SDIO_DRV_EDGE_SEL_SHIFT 23
#define SDIO_SAM_EDGE_SEL_SHIFT 25
#define SDIO_SLF_CLK_EN (1u << 27)
#define SDIO_DRV_CLK_EN (1u << 28)
#define SDIO_SAM_CLK_EN (1u << 29)
#define SDMMC_RESET_EN (1u << 28)

/* Same board-proven readiness window as the Stamp bootstrap. */
#define C6_RESET_ASSERT_US 10000u
#define C6_RESET_INACTIVE_US 10000u
#define C6_RESET_READY_DELAY_US 1500000u
#define C6_POWER_UP_DELAY_US 20000u

#define IOMUX_FUN_PD (1u << 7)
#define IOMUX_FUN_PU (1u << 8)
#define IOMUX_FUN_IE (1u << 9)
#define IOMUX_FUN_DRV_MASK (3u << 10)
#define IOMUX_MCU_SEL_MASK (7u << 12)
#define PIN_FUNC_GPIO 1u

/* Internal I2C bus: both PI4IOE5V6408 expanders, the codec, mic ADC,
 * RTC, touch and the power monitor share it (board-contract.json). */
#define TAB5_I2C_PORT 0
#define TAB5_I2C_SDA_GPIO 31
#define TAB5_I2C_SCL_GPIO 32
#define TAB5_I2C_HZ 400000u

/* PI4IOE5V6408 registers (esp_io_expander_pi4ioe5v6408.c and the stock
 * bsp_io_expander_pi4ioe_init in m5stack_tab5.c).  IO_DIR: 1=output,
 * 0=input -- the "0: input 1: output" comment at m5stack_tab5.c:303. */
#define PI4IO_REG_CHIP_RESET 0x01u
#define PI4IO_REG_IO_DIR 0x03u /* 1=output, 0=input */
#define PI4IO_REG_OUT_SET 0x05u
#define PI4IO_REG_OUT_H_IM 0x07u /* 1=high-Z output mode */
#define PI4IO_REG_IN_DEF_STA 0x09u
#define PI4IO_REG_PULL_EN 0x0Bu
#define PI4IO_REG_PULL_SEL 0x0Du /* 1=pull-up, 0=pull-down */
#define PI4IO_REG_INT_MASK 0x11u /* 1=irq masked */

#define EXPANDER0_ADDR 0x43u
#define EXPANDER1_ADDR 0x44u

/* 0x43 pins: P0 EXT_ANT_EN, P1 SPK_EN, P2 EXT5V_EN, P4 LCD_RST,
 * P5 TP_RST, P6 CAM_RST, P7 hp-detect(input).  Stock latches 0x66 and
 * drives P5 high; the Linux profile keeps speaker/EXT5V/camera off, so
 * only TP_RST is 1.  P4 stays latched 0 across the whole sequence so
 * the output phase pulls the 1.8 V reset line low without ever
 * push-pulling 3.3 V onto it (m5stack_tab5.c comment at the OUT_SET
 * write). */
#define EXP0_OUT_BOOT 0x20u /* P5 TP_RST released; all other outputs 0 */
#define EXP0_PULLS 0x7fu    /* stock: pull-up on P0..P6, none on P7 */
#define EXP0_DIR_ASSERT 0x7fu /* P0..P6 outputs -> P4 drives LCD_RST low */
#define EXP0_DIR_RELEASE 0x6fu /* P4 back to input -> pull-up releases */

/* 0x44 pins: P0 WLAN_PWR_EN, P3 USB5V_EN, P4 PWROFF(keep 0),
 * P5 nCHG_QC_EN (0 = QC on), P7 CHG_EN; P1/P2/P6 stay inputs. */
#define EXP1_OUT_INIT 0x09u /* stock init: P0|P3 high, charge off */
#define EXP1_OUT_BOOT 0x89u /* +P7: stock reaches this via
			     * bsp_set_charge_en(true) shortly after
			     * expander init ("set charge enable: true"
			     * in boot-log.txt). */
#define EXP1_DIR 0xb9u   /* stock: P0,P3,P4,P5,P7 out; P1,P2,P6 in */
#define EXP1_OUT_H_IM 0x06u /* stock: P1/P2 high-Z */
#define EXP1_PULL_SEL 0xb9u /* stock */
#define EXP1_PULL_EN 0xf9u  /* stock: pulls on P0,P3,P4,P5,P6,P7 */
#define EXP1_IN_DEF_STA 0x40u /* stock: P6 compare-default high */
#define EXP1_INT_MASK 0xbfu   /* stock: P6 unmasked */

static inline volatile uint32_t *reg32(uint32_t addr)
{
	return (volatile uint32_t *)(uintptr_t)addr;
}

static inline void set_bits(uint32_t addr, uint32_t mask)
{
	*reg32(addr) |= mask;
}

static inline void clear_set_bits(uint32_t addr, uint32_t clear, uint32_t set)
{
	uint32_t value = *reg32(addr);
	*reg32(addr) = (value & ~clear) | set;
}

/* P4 IO_MUX pad registers sit at (gpio + 1) * 4 for the whole GPIO0..54
 * range: GPIO0 -> +0x4, GPIO8 -> +0x24, GPIO12 -> +0x34, GPIO15 -> +0x40,
 * GPIO42 -> +0xac (vendor io_mux_reg.h; the same rule produces the
 * 0xac..0xc4 Stamp table in m2_sdmmc.c). */
static void configure_iomux_gpio(uint32_t gpio, bool pullup)
{
	uint32_t clear = IOMUX_FUN_PD | IOMUX_FUN_PU | IOMUX_FUN_IE |
			 IOMUX_FUN_DRV_MASK | IOMUX_MCU_SEL_MASK;
	uint32_t set = IOMUX_FUN_IE | (3u << 10) | (PIN_FUNC_GPIO << 12);
	if (pullup)
		set |= IOMUX_FUN_PU;
	clear_set_bits(IO_MUX_BASE + ((gpio + 1u) * 4u), clear, set);
}

static void route_output(uint32_t gpio, uint32_t signal)
{
	/* OEN_SEL=0 delegates output-enable to the SDMMC peripheral. */
	clear_set_bits(GPIO_FUNC_OUT_SEL_BASE + (gpio * 4u),
			       0x1ffu | (1u << 10) | (1u << 11), signal & 0x1ffu);
}

static void route_input(uint32_t signal, uint32_t gpio)
{
	/* SIG_IN_SEL=1 selects the GPIO matrix rather than the direct bypass. */
	*reg32(GPIO_FUNC_IN_SEL_BASE + (signal * 4u)) = (gpio & 0x3fu) | (1u << 7);
}

static void pulse_c6_reset(void)
{
	const uint32_t bit = 1u << 15u; /* GPIO15: C6 EN/reset, bank 0 */

	configure_iomux_gpio(15u, false);
	*reg32(GPIO_ENABLE_W1TS) = bit;
	*reg32(GPIO_OUT_W1TS) = bit;
	esp_rom_delay_us(C6_RESET_ASSERT_US);
	*reg32(GPIO_OUT_W1TC) = bit;
	esp_rom_delay_us(C6_RESET_INACTIVE_US);
	*reg32(GPIO_OUT_W1TS) = bit;
	esp_rom_delay_us(C6_RESET_READY_DELAY_US);
	ESP_LOGI(TAG, "C6 reset GPIO15 released");
}

static void enable_sdmmc_clock(void)
{
	set_bits(HP_SOC_CLK_CTRL1, SDMMC_SYS_CLK_EN);
	set_bits(LP_SDMMC_RESET, SDMMC_RESET_EN);
	clear_set_bits(LP_SDMMC_RESET, SDMMC_RESET_EN, 0);

	/* PLL_F160M / host divider 4 gives a conservative 40 MHz LS clock. */
	clear_set_bits(HP_PERI_CLK_CTRL01,
			       SDIO_LS_CLK_SRC_SEL | SDIO_HS_MODE, SDIO_LS_CLK_EN);

	uint32_t clear = (0xfu << SDIO_EDGE_L_SHIFT) |
			 (0xfu << SDIO_EDGE_H_SHIFT) |
			 (0xfu << SDIO_EDGE_N_SHIFT) |
			 (0x3u << SDIO_SLF_EDGE_SEL_SHIFT) |
			 (0x3u << SDIO_DRV_EDGE_SEL_SHIFT) |
			 (0x3u << SDIO_SAM_EDGE_SEL_SHIFT);
	uint32_t set = (3u << SDIO_EDGE_L_SHIFT) |
			 (1u << SDIO_EDGE_H_SHIFT) |
			 (3u << SDIO_EDGE_N_SHIFT) |
			 (1u << SDIO_DRV_EDGE_SEL_SHIFT) |
			 SDIO_SLF_CLK_EN | SDIO_DRV_CLK_EN | SDIO_SAM_CLK_EN;
	clear_set_bits(HP_PERI_CLK_CTRL02, clear, set);
	set_bits(HP_PERI_CLK_CTRL02, SDIO_EDGE_CFG_UPDATE);
	clear_set_bits(HP_PERI_CLK_CTRL02, SDIO_EDGE_CFG_UPDATE, 0);
}

/* --- I2C expanders --------------------------------------------------- */

static i2c_master_bus_handle_t s_i2c_bus;
static i2c_master_dev_handle_t s_exp0;
static i2c_master_dev_handle_t s_exp1;

static esp_err_t expander_write(i2c_master_dev_handle_t dev, uint8_t reg,
				uint8_t value)
{
	uint8_t buf[2] = { reg, value };
	return i2c_master_transmit(dev, buf, sizeof(buf), 100);
}

/*
 * Program both expanders following the stock sequence in
 * bsp_io_expander_pi4ioe_init (m5stack_tab5.c:245-330), with only the
 * output levels changed for the Linux power profile.
 *
 * 0x43: chip reset; pulls; OUT_SET latch (P4=0, TP_RST=1); OUT_H_IM=0
 *   (push-pull); IO_DIR=0x7f so P4 becomes an output and drives LCD_RST
 *   low for 10 ms; IO_DIR=0x6f returns P4 to an input whose internal
 *   pull-up releases the panel reset; stock waits 50 ms after release.
 *   Never latch P4 high: the line has no 1.8 V pull-up and must not be
 *   push-pull driven to 3.3 V.
 * 0x44: chip reset; IO_DIR=0xb9 (P0,P3,P4,P5,P7 outputs; P1,P2,P6
 *   inputs); OUT_H_IM / PULL_SEL / PULL_EN / IN_DEF_STA / INT_MASK at
 *   the stock values; OUT_SET=0x09 (C6 rail + USB-A 5 V on, charge off)
 *   then 0x89 to match the stock image after its charge-enable call.
 */
static esp_err_t expander_reset(i2c_master_dev_handle_t dev)
{
	uint8_t cmd = PI4IO_REG_CHIP_RESET;
	uint8_t ff[] = { PI4IO_REG_CHIP_RESET, 0xffu };
	uint8_t ack = 0;

	ESP_RETURN_ON_ERROR(i2c_master_transmit(dev, ff, sizeof(ff), 100),
			    TAG, "expander reset write");
	ESP_RETURN_ON_ERROR(i2c_master_transmit_receive(dev, &cmd, 1, &ack, 1,
							100),
			    TAG, "expander reset read");
	return ESP_OK;
}

static esp_err_t expanders_init(void)
{
	i2c_master_bus_config_t bus_cfg = {
		.clk_source = I2C_CLK_SRC_DEFAULT,
		.i2c_port = TAB5_I2C_PORT,
		.scl_io_num = TAB5_I2C_SCL_GPIO,
		.sda_io_num = TAB5_I2C_SDA_GPIO,
		.glitch_ignore_cnt = 7,
		.flags.enable_internal_pullup = true,
	};
	ESP_RETURN_ON_ERROR(i2c_new_master_bus(&bus_cfg, &s_i2c_bus), TAG,
			    "i2c bus failed");

	i2c_device_config_t dev_cfg = {
		.dev_addr_length = I2C_ADDR_BIT_LEN_7,
		.device_address = EXPANDER0_ADDR,
		.scl_speed_hz = TAB5_I2C_HZ,
	};
	ESP_RETURN_ON_ERROR(i2c_master_bus_add_device(s_i2c_bus, &dev_cfg, &s_exp0),
			    TAG, "expander 0x43 add failed");
	dev_cfg.device_address = EXPANDER1_ADDR;
	ESP_RETURN_ON_ERROR(i2c_master_bus_add_device(s_i2c_bus, &dev_cfg, &s_exp1),
			    TAG, "expander 0x44 add failed");

	/* --- 0x43: stock order, TP_RST high, speaker/EXT5V/camera off --- */
	ESP_RETURN_ON_ERROR(expander_reset(s_exp0), TAG, "exp0 reset");
	ESP_RETURN_ON_ERROR(expander_write(s_exp0, PI4IO_REG_PULL_SEL,
					   EXP0_PULLS), TAG, "exp0 pull_sel");
	ESP_RETURN_ON_ERROR(expander_write(s_exp0, PI4IO_REG_PULL_EN,
					   EXP0_PULLS), TAG, "exp0 pull_en");
	ESP_RETURN_ON_ERROR(expander_write(s_exp0, PI4IO_REG_OUT_SET,
					   EXP0_OUT_BOOT), TAG, "exp0 out_set");
	ESP_RETURN_ON_ERROR(expander_write(s_exp0, PI4IO_REG_OUT_H_IM, 0x00u),
			    TAG, "exp0 out_h_im");
	ESP_RETURN_ON_ERROR(expander_write(s_exp0, PI4IO_REG_IO_DIR,
					   EXP0_DIR_ASSERT),
			    TAG, "exp0 io_dir assert");
	esp_rom_delay_us(10000);
	ESP_RETURN_ON_ERROR(expander_write(s_exp0, PI4IO_REG_IO_DIR,
					   EXP0_DIR_RELEASE),
			    TAG, "exp0 io_dir release");
	esp_rom_delay_us(50000);

	/* --- 0x44: exact stock sequence, then charge-enable --- */
	ESP_RETURN_ON_ERROR(expander_reset(s_exp1), TAG, "exp1 reset");
	ESP_RETURN_ON_ERROR(expander_write(s_exp1, PI4IO_REG_IO_DIR, EXP1_DIR),
			    TAG, "exp1 io_dir");
	ESP_RETURN_ON_ERROR(expander_write(s_exp1, PI4IO_REG_OUT_H_IM,
					   EXP1_OUT_H_IM), TAG, "exp1 out_h_im");
	ESP_RETURN_ON_ERROR(expander_write(s_exp1, PI4IO_REG_PULL_SEL,
					   EXP1_PULL_SEL), TAG, "exp1 pull_sel");
	ESP_RETURN_ON_ERROR(expander_write(s_exp1, PI4IO_REG_PULL_EN,
					   EXP1_PULL_EN), TAG, "exp1 pull_en");
	ESP_RETURN_ON_ERROR(expander_write(s_exp1, PI4IO_REG_IN_DEF_STA,
					   EXP1_IN_DEF_STA),
			    TAG, "exp1 in_def_sta");
	ESP_RETURN_ON_ERROR(expander_write(s_exp1, PI4IO_REG_INT_MASK,
					   EXP1_INT_MASK), TAG, "exp1 int_mask");
	ESP_RETURN_ON_ERROR(expander_write(s_exp1, PI4IO_REG_OUT_SET,
					   EXP1_OUT_INIT), TAG, "exp1 out_set");
	ESP_RETURN_ON_ERROR(expander_write(s_exp1, PI4IO_REG_OUT_SET,
					   EXP1_OUT_BOOT),
			    TAG, "exp1 charge en");

	esp_rom_delay_us(C6_POWER_UP_DELAY_US);
	ESP_LOGI(TAG, "expanders: C6 rail on, LCD_RST pulsed+released, "
		 "amp/EXT5V/camera off");
	return ESP_OK;
}

/*
 * Stock bsp_reset_tp() (m5stack_tab5.c), step for step:
 *   - GPIO23 (the touch INT line) goes back to a plain input with the
 *     default pull-up;
 *   - both reset latches (0x43 P4 LCD_RST, P5 TP_RST) are written 0 and
 *     P4 becomes an output, so both lines are low for 100 ms;
 *   - TP_RST is latched 1, P4 goes back to an input (the expander's
 *     pull-up releases the 1.8 V LCD reset rather than push-pulling 3.3 V
 *     onto it), and the stock waits another 100 ms.
 * Every register is a read-modify-write so the other pins keep their
 * levels.  Failures are logged and ignored: the detection that follows
 * reports whether the controller came back.
 */
void tab5_panel_reset(void)
{
	uint8_t reg, val;

	if (!s_exp0) {
		ESP_LOGW(TAG, "panel reset: expander 0x43 not up, skipped");
		return;
	}
	gpio_reset_pin(GPIO_NUM_23);

	reg = PI4IO_REG_OUT_SET;
	if (i2c_master_transmit_receive(s_exp0, &reg, 1, &val, 1, 100) != ESP_OK)
		goto fail;
	val &= (uint8_t)~((1u << 4) | (1u << 5));
	if (expander_write(s_exp0, PI4IO_REG_OUT_SET, val) != ESP_OK)
		goto fail;
	reg = PI4IO_REG_IO_DIR;
	if (i2c_master_transmit_receive(s_exp0, &reg, 1, &val, 1, 100) != ESP_OK)
		goto fail;
	val |= (1u << 4);
	if (expander_write(s_exp0, PI4IO_REG_IO_DIR, val) != ESP_OK)
		goto fail;
	esp_rom_delay_us(100000);

	reg = PI4IO_REG_OUT_SET;
	if (i2c_master_transmit_receive(s_exp0, &reg, 1, &val, 1, 100) != ESP_OK)
		goto fail;
	val &= (uint8_t)~(1u << 4);
	val |= (1u << 5);
	if (expander_write(s_exp0, PI4IO_REG_OUT_SET, val) != ESP_OK)
		goto fail;
	reg = PI4IO_REG_IO_DIR;
	if (i2c_master_transmit_receive(s_exp0, &reg, 1, &val, 1, 100) != ESP_OK)
		goto fail;
	val &= (uint8_t)~(1u << 4);
	if (expander_write(s_exp0, PI4IO_REG_IO_DIR, val) != ESP_OK)
		goto fail;
	esp_rom_delay_us(100000);
	ESP_LOGI(TAG, "panel reset: LCD_RST+TP_RST pulsed 100 ms, released");
	return;
fail:
	ESP_LOGW(TAG, "panel reset: expander access failed; continuing");
}

/*
 * The touch controller's firmware byte: 16-bit register address 0x0000
 * (big endian on the wire), one byte back, at 100 kHz like the BSP.  The
 * controller needs a moment after TP_RST is released, so retry for about
 * 400 ms before calling it absent.
 */
#define TAB5_TOUCH_ADDR 0x55u
#define TAB5_TOUCH_HZ 100000u
#define TAB5_FW_ST7121 1u
#define TAB5_FW_ST7123 3u

enum tab5_panel tab5_detect_panel(uint8_t *fw_out)
{
	i2c_device_config_t dev_cfg = {
		.dev_addr_length = I2C_ADDR_BIT_LEN_7,
		.device_address = TAB5_TOUCH_ADDR,
		.scl_speed_hz = TAB5_TOUCH_HZ,
	};
	i2c_master_dev_handle_t tp = NULL;
	const uint8_t reg[2] = { 0x00u, 0x00u };
	uint8_t fw = 0;
	bool answered = false;

	if (fw_out)
		*fw_out = 0;
	if (!s_i2c_bus ||
	    i2c_master_bus_add_device(s_i2c_bus, &dev_cfg, &tp) != ESP_OK) {
		ESP_LOGW(TAG, "panel detect: cannot open I2C device 0x%02x",
			 TAB5_TOUCH_ADDR);
		return TAB5_PANEL_NONE;
	}
	for (int i = 0; i < 20 && !answered; ++i) {
		if (i2c_master_transmit_receive(tp, reg, sizeof(reg), &fw, 1,
						100) == ESP_OK)
			answered = true;
		else
			esp_rom_delay_us(20000);
	}
	i2c_master_bus_rm_device(tp);

	if (!answered) {
		ESP_LOGW(TAG, "panel detect: nothing answers at 0x%02x (reg 0x0000)",
			 TAB5_TOUCH_ADDR);
		return TAB5_PANEL_NONE;
	}
	if (fw_out)
		*fw_out = fw;
	if (fw == TAB5_FW_ST7121) {
		ESP_LOGI(TAG, "panel detect: touch FW byte %u -> ST7121", fw);
		return TAB5_PANEL_ST7121;
	}
	if (fw == TAB5_FW_ST7123) {
		ESP_LOGI(TAG, "panel detect: touch FW byte %u -> ST7123", fw);
		return TAB5_PANEL_ST7123;
	}
	ESP_LOGW(TAG, "panel detect: unknown touch FW byte %u, BSP fallback "
		 "-> ST7123", fw);
	return TAB5_PANEL_ST7123;
}

/*
 * #56 hook: deep_sleep.c calls this where the Stamp board pulses the
 * GPIO42 C6 latch.  The Tab5 C6 rail is expander 0x44 P0, reached over
 * the internal I2C bus; the bus is brought up here because
 * deep_sleep_early() runs before expanders_init().  P0 is cleared with a
 * read-modify-write so the other rails keep their levels, and the bus is
 * torn down again so expanders_init() can take it in the boot that did
 * not sleep.  The next boot's expander init turns the rail back on.
 */
void tab5_c6_power_off(void)
{
	i2c_master_bus_handle_t bus = s_i2c_bus;
	i2c_master_dev_handle_t dev = s_exp1;
	uint8_t reg = PI4IO_REG_OUT_SET;
	uint8_t out = 0;
	bool own_bus = false;

	if (!bus) {
		i2c_master_bus_config_t bus_cfg = {
			.clk_source = I2C_CLK_SRC_DEFAULT,
			.i2c_port = TAB5_I2C_PORT,
			.scl_io_num = TAB5_I2C_SCL_GPIO,
			.sda_io_num = TAB5_I2C_SDA_GPIO,
			.glitch_ignore_cnt = 7,
			.flags.enable_internal_pullup = true,
		};
		i2c_device_config_t dev_cfg = {
			.dev_addr_length = I2C_ADDR_BIT_LEN_7,
			.device_address = EXPANDER1_ADDR,
			.scl_speed_hz = TAB5_I2C_HZ,
		};

		if (i2c_new_master_bus(&bus_cfg, &bus) != ESP_OK ||
		    i2c_master_bus_add_device(bus, &dev_cfg, &dev) != ESP_OK) {
			ESP_LOGE(TAG, "c6 off: i2c bus failed");
			return;
		}
		own_bus = true;
	}
	if (i2c_master_transmit_receive(dev, &reg, 1, &out, 1, 100) == ESP_OK &&
	    expander_write(dev, PI4IO_REG_OUT_SET, out & ~0x01u) == ESP_OK)
		ESP_LOGI(TAG, "c6 rail off (expander 0x44 P0)");
	else
		ESP_LOGE(TAG, "c6 off: expander write failed");

	if (own_bus) {
		i2c_master_bus_rm_device(dev);
		i2c_del_master_bus(bus);
	}
}

/*
 * Last board step before the jump: take the internal I2C bus away from
 * IDF so nothing on it survives into Linux.
 *
 * Order (everything after the expander reads with mstatus.MIE clear):
 *   0. latch the expander output registers so the log can show the
 *      real TP_RST / C6-rail state at hand-off -- the blocking I2C
 *      API needs the I2C0 interrupt route, so read before detaching;
 *   1. detach the I2C0 source (INTMTX map +0xb0, src 44) so it cannot
 *      reach any CLIC slot, and disarm the slot it pointed at -- the
 *      same class as the display pair in tab5_display_status();
 *   2. i2c_master_bus_rm_device() the two expander handles first:
 *      i2c_del_master_bus() returns ESP_ERR_INVALID_STATE while its
 *      device list is non-empty (i2c_master.c), which is exactly the
 *      failure the first version of this function hit;
 *   3. i2c_del_master_bus() -- frees the IDF CPU interrupt and runs
 *      i2c_common_deinit_pins(): open-drain off, output-enable off,
 *      oen_sel back to the ENABLE bit, PIN_FUNC_GPIO selected, I2C
 *      input-matrix links zeroed.  If the delete still fails the same
 *      pin work is done by hand so Linux gets plain pads anyway;
 *   4. force OUT_SEL back to the plain-GPIO signal (SIG_GPIO_OUT_IDX,
 *      0x100): the deinit path restores the OE control but never the
 *      routed output signal, so GPIO31/32 would keep listening to the
 *      I2C0 peripheral's signals (0x44/0x45) -- harmless while their
 *      ENABLE bits are clear, but the Linux bit-banger also toggles
 *      only OUT/ENABLE, so the pads must be plain GPIO end-to-end;
 *   5. GPIO23 (ST712x INT) to input + pull-up + intr off;
 *   6. read everything back and print, in tab5_display_status() style.
 *
 * TP_RST stays released (expander 0x43 P5 output high); the touch
 * controller is never reset after detection and keeps its rail.
 */
void tab5_i2c_release(void)
{
	esp_err_t rm0 = ESP_OK, rm1 = ESP_OK, del = ESP_OK;
	uint32_t route, i2c_slot, route_rb;
	uint32_t outsel31, outsel32, en31, en32;
	uint8_t rd = PI4IO_REG_OUT_SET, out0 = 0, out1 = 0;

	/*
	 * Read the expander output latches first: the blocking I2C API
	 * still needs the I2C0 interrupt route live, so this must happen
	 * before the detach below.  P5 of expander 0x43 is TP_RST (held
	 * high); P0 of expander 0x44 is the C6 rail.
	 */
	bool have0 = s_exp0 &&
		i2c_master_transmit_receive(s_exp0, &rd, 1, &out0, 1, 100) == ESP_OK;
	bool have1 = s_exp1 &&
		i2c_master_transmit_receive(s_exp1, &rd, 1, &out1, 1, 100) == ESP_OK;

	unsigned long mie = RV_CLEAR_CSR(mstatus, MSTATUS_MIE) & MSTATUS_MIE;

	route = REG_READ(INTERRUPT_CORE0_I2C0_INT_MAP_REG);
	i2c_slot = route & 0x3f;
	REG_WRITE(INTERRUPT_CORE0_I2C0_INT_MAP_REG, 0);
	if (i2c_slot) {
		*(volatile uint8_t *)BYTE_CLIC_INT_IE_REG(i2c_slot) = 0;
		*(volatile uint8_t *)BYTE_CLIC_INT_IP_REG(i2c_slot) = 0;
	}

	if (s_exp0) {
		rm0 = i2c_master_bus_rm_device(s_exp0);
		s_exp0 = NULL;
	}
	if (s_exp1) {
		rm1 = i2c_master_bus_rm_device(s_exp1);
		s_exp1 = NULL;
	}
	if (rm0 != ESP_OK || rm1 != ESP_OK)
		ESP_LOGE(TAG, "i2c release: rm_device failed: 0x43=%s 0x44=%s",
			 esp_err_to_name(rm0), esp_err_to_name(rm1));

	if (!s_i2c_bus) {
		ESP_LOGW(TAG, "i2c release: bus was never up; pads forced "
			 "to plain GPIO anyway");
	} else {
		del = i2c_del_master_bus(s_i2c_bus);
		if (del == ESP_OK) {
			s_i2c_bus = NULL;
		} else {
			ESP_LOGE(TAG, "i2c release: i2c_del_master_bus=%s; "
				 "restoring SDA/SCL pads by hand",
				 esp_err_to_name(del));
			for (uint32_t pin = TAB5_I2C_SDA_GPIO;
			     pin <= TAB5_I2C_SCL_GPIO; ++pin) {
				gpio_od_disable(pin);
				gpio_output_disable(pin);
			}
		}
	}
	/* deinit does not rewrite OUT_SEL; force the plain-GPIO signal. */
	esp_rom_gpio_connect_out_signal(TAB5_I2C_SDA_GPIO, SIG_GPIO_OUT_IDX,
					false, false);
	esp_rom_gpio_connect_out_signal(TAB5_I2C_SCL_GPIO, SIG_GPIO_OUT_IDX,
					false, false);
	route_rb = REG_READ(INTERRUPT_CORE0_I2C0_INT_MAP_REG);

	if (mie)
		RV_SET_CSR(mstatus, MSTATUS_MIE);

	gpio_set_direction(GPIO_NUM_23, GPIO_MODE_INPUT);
	gpio_set_pull_mode(GPIO_NUM_23, GPIO_PULLUP_ONLY);
	gpio_set_intr_type(GPIO_NUM_23, GPIO_INTR_DISABLE);

	outsel31 = *reg32(GPIO_FUNC_OUT_SEL_BASE + TAB5_I2C_SDA_GPIO * 4u) &
		0x1ffu;
	outsel32 = *reg32(GPIO_FUNC_OUT_SEL_BASE + TAB5_I2C_SCL_GPIO * 4u) &
		0x1ffu;
	en31 = (*reg32(GPIO_BASE + 0x20u) >> TAB5_I2C_SDA_GPIO) & 1u;
	en32 = (*reg32(GPIO_BASE + 0x2cu) >> (TAB5_I2C_SCL_GPIO - 32u)) & 1u;

	ESP_LOGI(TAG, "i2c release: rm 0x43=%s 0x44=%s del=%s, "
		 "I2C0 route %" PRIu32 "->%" PRIu32 " slot %" PRIu32
		 " IE+IP cleared; outsel31=%03" PRIx32
		 " outsel32=%03" PRIx32 " en31=%" PRIu32 " en32=%" PRIu32
		 "; GPIO23 input+pu lvl=%d",
		 esp_err_to_name(rm0), esp_err_to_name(rm1),
		 esp_err_to_name(del), i2c_slot, route_rb & 0x3f, i2c_slot,
		 outsel31, outsel32, en31, en32,
		 gpio_get_level(GPIO_NUM_23));
	ESP_LOGI(TAG, "i2c release: exp0 out=%s0x%02x P5/TP_RST=%d, "
		 "exp1 out=%s0x%02x P0/C6rail=%d -- touch rail left on",
		 have0 ? "" : "?!", out0, (out0 >> 5) & 1,
		 have1 ? "" : "?!", out1, out1 & 1);
}

void tab5_board_init(struct tab5_fb *fb)
{
	fb->ok = false;
	fb->pa = 0;
	fb->bytes = 0;

	/* Backlight off before anything else touches the display rails. */
	gpio_config_t bl = {
		.pin_bit_mask = 1ull << 22,
		.mode = GPIO_MODE_OUTPUT,
		.pull_up_en = GPIO_PULLUP_DISABLE,
		.pull_down_en = GPIO_PULLDOWN_DISABLE,
		.intr_type = GPIO_INTR_DISABLE,
	};
	gpio_config(&bl);
	gpio_set_level(GPIO_NUM_22, 0);

	if (expanders_init() != ESP_OK) {
		ESP_LOGE(TAG, "expander init failed; leaving rails at reset defaults "
			 "(C6 unpowered, panel in reset) and continuing headless");
	} else {
		enable_sdmmc_clock();

		/* Slot 1 via the matrix: CLK=12 CMD=13 DAT0..3={11,10,9,8}. */
		const uint32_t pins[] = { 12u, 13u, 11u, 10u, 9u, 8u };
		for (uint32_t i = 0; i < 6u; ++i)
			configure_iomux_gpio(pins[i], i != 0u);

		route_output(12u, 0u); /* SD_CARD_CCLK_2_PAD_OUT_IDX */
		route_output(13u, 1u); /* SD_CARD_CCMD_2_PAD_OUT_IDX */
		route_output(11u, 2u);
		route_output(10u, 3u);
		route_output(9u, 4u);
		route_output(8u, 5u);
		route_input(1u, 13u); /* CMD */
		route_input(2u, 11u); /* DAT0 */
		route_input(3u, 10u); /* DAT1 / SDIO interrupt */
		route_input(4u, 9u);  /* DAT2 */
		route_input(5u, 8u);  /* DAT3 */
		/* Same slot-1 card-int tie-off as the Stamp path. */
		route_input(129u, 0x3fu);

		pulse_c6_reset();
		ESP_LOGI(TAG, "Slot 1 SDIO ready on GPIO8-13; C6 reset done");

		tab5_display_init(fb);
	}
}
