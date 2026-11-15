// SPDX-License-Identifier: Apache-2.0 OR GPL-2.0-only
/*
 * M5Stack Tab5 OTA host: switch the ESP32-C6 supply on before ESP-Hosted
 * brings up the SDIO link.
 *
 * Derived from linux/boot-shim/main/tab5_power.c of
 * https://github.com/Riti0208/tab5-linux (SPDX Apache-2.0 OR GPL-2.0-only,
 * a fork of the Stamp-P4 Linux port), which that project ran on a Tab5 on
 * 2026-10-18 ("tab5-power: C6 power on (expander 0x44 id 0xa0,
 * WLAN_PWR_EN)").  Reduced here to the C6 path and wrapped in a constructor.
 *
 * The C6's 3.3 V comes from a load switch whose enable is WLAN_PWR_EN, P0
 * of the PI4IOE5V6408 at I2C 0x44.  The 0x43 expander drives
 * RF_PTH_L_INT_H_EXT on P0 (the C6 antenna switch; low = internal).  Both
 * sit on the system I2C bus (SDA GPIO31, SCL GPIO32) and are reset together
 * with the P4, so every boot of this host starts with the C6 unpowered.
 * This host is a separate application from the Linux boot shim (which does
 * the same job in tab5_board.c), so it has to do it itself.
 *
 * ESP-Hosted-MCU starts its transport from a default-priority constructor,
 * before the scheduler runs.  Prioritised constructors run first, so
 * constructor(101) powers the C6 before ESP-Hosted resets it and
 * enumerates the SDIO card.  That is also why the bus is bit-banged with
 * open-drain GPIO instead of the I2C driver.  The pins are released
 * (inputs) on return, and only the expander bits named above change; every
 * other expander pin keeps its reset state.
 */

#include <stdbool.h>
#include <stdint.h>

#include "driver/gpio.h"
#include "esp_log.h"
#include "esp_rom_sys.h"

static const char *TAG = "tab5-c6-power";

#define TAB5_I2C_SDA GPIO_NUM_31
#define TAB5_I2C_SCL GPIO_NUM_32
#define I2C_HALF_PERIOD_US 5u /* about 100 kHz */

#define PI4IO_ADDR_SYS 0x43u /* LCD/touch/camera resets, RF switch */
#define PI4IO_ADDR_PWR 0x44u /* WLAN power, USB-A 5 V, charger */

#define PI4IO_REG_DEVICE_ID 0x01u
#define PI4IO_REG_IO_DIR 0x03u    /* 1 = output */
#define PI4IO_REG_OUT 0x05u
#define PI4IO_REG_OUT_HIGHZ 0x07u /* 1 = output disabled (reset 0xff) */

#define PI4IO_PWR_WLAN_PWR_EN (1u << 0)
#define PI4IO_SYS_RF_PTH_EXT (1u << 0)

/* Settle time after the load switch turns on, before the reset sequence. */
#define C6_POWER_SETTLE_US 50000u

static void line_release(gpio_num_t pin)
{
	gpio_set_level(pin, 1);
}

static void line_low(gpio_num_t pin)
{
	gpio_set_level(pin, 0);
}

static void i2c_delay(void)
{
	esp_rom_delay_us(I2C_HALF_PERIOD_US);
}

static void i2c_start(void)
{
	line_release(TAB5_I2C_SDA);
	line_release(TAB5_I2C_SCL);
	i2c_delay();
	line_low(TAB5_I2C_SDA);
	i2c_delay();
	line_low(TAB5_I2C_SCL);
	i2c_delay();
}

static void i2c_stop(void)
{
	line_low(TAB5_I2C_SDA);
	i2c_delay();
	line_release(TAB5_I2C_SCL);
	i2c_delay();
	line_release(TAB5_I2C_SDA);
	i2c_delay();
}

static bool i2c_write_byte(uint8_t value)
{
	for (int bit = 7; bit >= 0; --bit) {
		if (value & (1u << bit))
			line_release(TAB5_I2C_SDA);
		else
			line_low(TAB5_I2C_SDA);
		i2c_delay();
		line_release(TAB5_I2C_SCL);
		i2c_delay();
		line_low(TAB5_I2C_SCL);
	}
	line_release(TAB5_I2C_SDA);
	i2c_delay();
	line_release(TAB5_I2C_SCL);
	i2c_delay();
	bool ack = gpio_get_level(TAB5_I2C_SDA) == 0;
	line_low(TAB5_I2C_SCL);
	i2c_delay();
	return ack;
}

static uint8_t i2c_read_byte_nack(void)
{
	uint8_t value = 0;

	line_release(TAB5_I2C_SDA);
	for (int bit = 7; bit >= 0; --bit) {
		i2c_delay();
		line_release(TAB5_I2C_SCL);
		i2c_delay();
		if (gpio_get_level(TAB5_I2C_SDA))
			value |= 1u << bit;
		line_low(TAB5_I2C_SCL);
	}
	/* NACK: leave SDA released for the ninth clock. */
	i2c_delay();
	line_release(TAB5_I2C_SCL);
	i2c_delay();
	line_low(TAB5_I2C_SCL);
	i2c_delay();
	return value;
}

static bool pi4io_read(uint8_t addr, uint8_t reg, uint8_t *value)
{
	bool ok;

	i2c_start();
	ok = i2c_write_byte(addr << 1) && i2c_write_byte(reg);
	if (ok) {
		i2c_start(); /* repeated start */
		ok = i2c_write_byte((addr << 1) | 1u);
		if (ok)
			*value = i2c_read_byte_nack();
	}
	i2c_stop();
	return ok;
}

static bool pi4io_write(uint8_t addr, uint8_t reg, uint8_t value)
{
	bool ok;

	i2c_start();
	ok = i2c_write_byte(addr << 1) && i2c_write_byte(reg) &&
	     i2c_write_byte(value);
	i2c_stop();
	return ok;
}

/* Drive the given expander pins as outputs at the given level. */
static bool pi4io_drive(uint8_t addr, uint8_t mask, bool high)
{
	uint8_t out, highz, dir;

	if (!pi4io_read(addr, PI4IO_REG_OUT, &out) ||
	    !pi4io_read(addr, PI4IO_REG_OUT_HIGHZ, &highz) ||
	    !pi4io_read(addr, PI4IO_REG_IO_DIR, &dir))
		return false;
	out = high ? (out | mask) : (out & ~mask);
	/* Level first, then enable the driver, so the pin never glitches. */
	return pi4io_write(addr, PI4IO_REG_OUT, out) &&
	       pi4io_write(addr, PI4IO_REG_OUT_HIGHZ, highz & ~mask) &&
	       pi4io_write(addr, PI4IO_REG_IO_DIR, dir | mask);
}

static void i2c_bus_claim(void)
{
	const gpio_config_t cfg = {
		.pin_bit_mask = (1ull << TAB5_I2C_SDA) | (1ull << TAB5_I2C_SCL),
		.mode = GPIO_MODE_INPUT_OUTPUT_OD,
		.pull_up_en = GPIO_PULLUP_ENABLE,
		.pull_down_en = GPIO_PULLDOWN_DISABLE,
		.intr_type = GPIO_INTR_DISABLE,
	};

	gpio_set_level(TAB5_I2C_SDA, 1);
	gpio_set_level(TAB5_I2C_SCL, 1);
	gpio_config(&cfg);
	/* Clock out a device that may hold SDA from an interrupted transfer. */
	for (int i = 0; i < 9 && gpio_get_level(TAB5_I2C_SDA) == 0; ++i) {
		line_low(TAB5_I2C_SCL);
		i2c_delay();
		line_release(TAB5_I2C_SCL);
		i2c_delay();
	}
	i2c_stop();
}

static void i2c_bus_release(void)
{
	gpio_set_direction(TAB5_I2C_SDA, GPIO_MODE_INPUT);
	gpio_set_direction(TAB5_I2C_SCL, GPIO_MODE_INPUT);
}

static void __attribute__((constructor(101))) tab5_c6_power_on(void)
{
	uint8_t id = 0;
	bool ok;

	i2c_bus_claim();
	ok = pi4io_read(PI4IO_ADDR_PWR, PI4IO_REG_DEVICE_ID, &id);
	if (!ok) {
		ESP_LOGE(TAG, "no PI4IOE5V6408 at 0x%02x; C6 stays unpowered",
			 PI4IO_ADDR_PWR);
		i2c_bus_release();
		return;
	}
	if (!pi4io_drive(PI4IO_ADDR_SYS, PI4IO_SYS_RF_PTH_EXT, false))
		ESP_LOGW(TAG, "antenna switch at 0x%02x not set; keeping its default",
			 PI4IO_ADDR_SYS);
	ok = pi4io_drive(PI4IO_ADDR_PWR, PI4IO_PWR_WLAN_PWR_EN, true);
	i2c_bus_release();
	if (!ok) {
		ESP_LOGE(TAG, "WLAN_PWR_EN write failed; C6 stays unpowered");
		return;
	}
	esp_rom_delay_us(C6_POWER_SETTLE_US);
	ESP_LOGI(TAG, "C6 power on (expander 0x%02x id 0x%02x, WLAN_PWR_EN)",
		 PI4IO_ADDR_PWR, id);
}
