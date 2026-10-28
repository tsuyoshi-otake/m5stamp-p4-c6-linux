// SPDX-License-Identifier: Apache-2.0 OR GPL-2.0-only
/*
 * Deep Sleep for Linux (#56).
 *
 * deep_sleep_early() runs first in app_main().  It works out how this boot
 * started, takes the sleep memory Linux left in PSRAM into LP RAM, and, if
 * Linux left a sleep request, turns the C6 off and enters Deep Sleep.
 * deep_sleep_publish() runs just before the jump to Linux and writes the wake
 * report and the sleep memory to the PSRAM mailbox.
 *
 * On chip rev v1.3 a wake from Deep Sleep resets with HP_SYS_HP_WDT_RESET,
 * the same reason as a Linux reboot, and the PMU wakeup status survives other
 * resets.  So a wake is recognised by the LP RAM state written just before
 * sleeping, not by the reset reason (#56 hardware results).
 *
 * A request can also run the LP core while the chip sleeps (#57): the program
 * in lp/lp_monitor.c reads the LP temperature sensor every lp_period_ms and
 * wakes the chip when the temperature passes a threshold.  The thresholds go
 * to it as raw sensor steps, converted with the reading taken just before the
 * sleep, and its results come back as milli-degC the same way.
 *
 * Linux can also load a user program into the LP core (#59) and keep it
 * running, or have it started here for the sleep.  The request then says so
 * (SLEEP_FLAG_LP_KEEP / SLEEP_FLAG_LP_RUN) and the program may wake the chip.
 */

#include <inttypes.h>
#include <stdbool.h>
#include <stdint.h>
#include <string.h>

#include "driver/gpio.h"
#include "driver/rtc_io.h"
#include "driver/temperature_sensor.h"
#include "esp_attr.h"
#include "esp_private/esp_sleep_internal.h"
#include "esp_rom_sys.h"
#include "esp_rtc_time.h"
#include "esp_sleep.h"
#include "esp_timer.h"
#include "esp32p4/rom/ets_sys.h"
#include "hal/cache_hal.h"
#include "hal/rtc_hal.h"
#include "hal/temperature_sensor_ll.h"
#include "soc/lp_system_reg.h"
#include "soc/reset_reasons.h"
#include "ulp_lp_core.h"
#include "ulp_lp_core_memory_shared.h"

#include "boot_ab.h"
#include "deep_sleep.h"
#include "lp_monitor.h"

extern const uint8_t lp_monitor_bin_start[] asm("_binary_lp_monitor_bin_start");
extern const uint8_t lp_monitor_bin_end[] asm("_binary_lp_monitor_bin_end");
extern uint32_t _rtc_ulp_memory_start;

/* One sensor step is 0.4386 degC (TEMPERATURE_SENSOR_LL_ADC_FACTOR). */
#define TSENS_MC_PER_STEP_X10 4386

#define LP_STATE_MAGIC  0x45534c50u /* "ESLP" */
#define LP_MEM_MAGIC    0x45534c4du /* "ESLM" */
#define C6_OFF_PULSE_US 100000u

/* Shim-private LP RAM: only this file reads it, so its address is free. */
struct lp_sleep_state {
	uint32_t magic;
	struct sleep_request req;
	uint32_t rtc_us_lo;     /* esp_rtc_get_time_us() just before sleeping */
	uint32_t rtc_us_hi;
	uint32_t boot_us;       /* esp_timer_get_time() then: since the reset */
	int32_t temp_base_mc;   /* milli-degC = raw * 438.6 + temp_base_mc */
	uint32_t crc32;
} __attribute__((packed));

struct lp_sleep_mem {
	uint32_t magic;
	uint32_t len;
	uint32_t crc32;
	uint8_t data[SLEEP_MEM_BYTES];
} __attribute__((packed));

static RTC_NOINIT_ATTR struct lp_sleep_state lp_state;
static RTC_NOINIT_ATTR struct lp_sleep_mem lp_mem;

/* The report for this boot; published just before the Linux jump. */
static struct wake_info report;
static bool wall_known;
static uint64_t wall_us_at_sleep;
static uint64_t rtc_us_at_sleep;
/* The LP core thresholds for the coming sleep (check_request). */
static int32_t temp_base_mc;
static uint32_t temp_low_raw, temp_high_raw;

static uint32_t crc32_ieee(const void *data, uint32_t bytes)
{
	const uint8_t *p = data;
	uint32_t crc = 0xffffffffu;

	while (bytes--) {
		crc ^= *p++;
		for (unsigned int bit = 0; bit < 8; ++bit)
			crc = (crc >> 1) ^ ((crc & 1u) ? 0xedb88320u : 0u);
	}
	return crc ^ 0xffffffffu;
}

static bool lp_state_valid(void)
{
	return lp_state.magic == LP_STATE_MAGIC &&
	       lp_state.crc32 == crc32_ieee(&lp_state,
					    offsetof(struct lp_sleep_state, crc32));
}

static bool lp_mem_valid(void)
{
	return lp_mem.magic == LP_MEM_MAGIC && lp_mem.len <= SLEEP_MEM_BYTES &&
	       lp_mem.crc32 == crc32_ieee(lp_mem.data, lp_mem.len);
}

static void psram_read(uint32_t pa, void *dst, uint32_t bytes)
{
	cache_hal_invalidate_addr(pa, bytes);
	memcpy(dst, (const void *)(uintptr_t)pa, bytes);
}

static void psram_write(uint32_t pa, const void *src, uint32_t bytes)
{
	memcpy((void *)(uintptr_t)pa, src, bytes);
	cache_hal_writeback_addr(pa, bytes);
}

/* Whether boot-commit left a /boot update for main.c to apply. */
static bool boot_update_pending(void)
{
	struct boot_update_request r;

	psram_read(BOOT_UPDATE_REQUEST_PA, &r, sizeof(r));
	return r.magic == BOOT_REQUEST_MAGIC && r.commit_word == BOOT_REQUEST_COMMIT;
}

static bool button_held(void)
{
	gpio_config_t c = {
		.pin_bit_mask = 1ULL << BOARD_SLEEP_BUTTON_GPIO,
		.mode = GPIO_MODE_INPUT,
		.pull_up_en = GPIO_PULLUP_ENABLE,
	};

	gpio_config(&c);
	esp_rom_delay_us(1000);
	return gpio_get_level(BOARD_SLEEP_BUTTON_GPIO) == 0;
}

/* GPIO42 latches the C6 rail; a low pulse turns it off and it stays off. */
static void c6_power_off(void)
{
	gpio_config_t c = {
		.pin_bit_mask = 1ULL << BOARD_SLEEP_C6_POWER_GPIO,
		.mode = GPIO_MODE_INPUT_OUTPUT,
	};

	gpio_config(&c);
	gpio_set_level(BOARD_SLEEP_C6_POWER_GPIO, 0);
	esp_rom_delay_us(C6_OFF_PULSE_US);
	c.mode = GPIO_MODE_INPUT;
	gpio_config(&c);
}

static int32_t raw_to_mc(uint32_t raw, int32_t base)
{
	return (int32_t)((int64_t)raw * TSENS_MC_PER_STEP_X10 / 10) + base;
}

/* floor(a / b) and ceil(a / b) for b > 0. */
static int64_t div_floor(int64_t a, int64_t b)
{
	return a >= 0 ? a / b : -((-a + b - 1) / b);
}

static int64_t div_ceil(int64_t a, int64_t b)
{
	return -div_floor(-a, b);
}

/*
 * Read the sensor and turn the request's thresholds into raw steps.  Returns
 * a skip reason when the temperature is already past one, as a wake pin at
 * its level is.
 */
static uint32_t temp_prepare(const struct sleep_request *req)
{
	temperature_sensor_config_t cfg = TEMPERATURE_SENSOR_CONFIG_DEFAULT(-10, 80);
	temperature_sensor_handle_t h = NULL;
	float c;
	uint32_t raw;
	int32_t now_mc;

	if (temperature_sensor_install(&cfg, &h) != ESP_OK ||
	    temperature_sensor_enable(h) != ESP_OK ||
	    temperature_sensor_get_celsius(h, &c) != ESP_OK)
		return SLEEP_SKIP_BAD_REQUEST;
	raw = temperature_sensor_ll_get_raw_value();
	if (raw == 0)
		return SLEEP_SKIP_BAD_REQUEST;
	/* The sensor stays enabled: the LP core reads it while asleep. */
	now_mc = (int32_t)(c * 1000.0f);
	temp_base_mc = now_mc - (int32_t)((int64_t)raw * TSENS_MC_PER_STEP_X10 / 10);
	temp_low_raw = 0;
	temp_high_raw = 0;
	if (req->lp_temp_low_mc != SLEEP_TEMP_OFF) {
		int64_t r = div_floor(((int64_t)req->lp_temp_low_mc - temp_base_mc) * 10,
				      TSENS_MC_PER_STEP_X10);

		if (r >= raw)
			return SLEEP_SKIP_TEMP_ACTIVE;
		/* Below the sensor's range it can never fire: leave it off. */
		temp_low_raw = r >= 1 ? (uint32_t)r : 0;
	}
	if (req->lp_temp_high_mc != SLEEP_TEMP_OFF) {
		int64_t r = div_ceil(((int64_t)req->lp_temp_high_mc - temp_base_mc) * 10,
				     TSENS_MC_PER_STEP_X10);

		if (r <= raw)
			return SLEEP_SKIP_TEMP_ACTIVE;
		temp_high_raw = r <= 255 ? (uint32_t)r : 0;
	}
	ets_printf("boot: DEEP_SLEEP temp now=%d mC raw=%u low_raw=%u high_raw=%u period_ms=%u\n",
		   (int)now_mc, (unsigned)raw, (unsigned)temp_low_raw,
		   (unsigned)temp_high_raw, (unsigned)req->lp_period_ms);
	return SLEEP_SKIP_NONE;
}

static bool lp_start(const struct sleep_request *req)
{
	ulp_lp_core_cfg_t cfg = {
		.wakeup_source = ULP_LP_CORE_WAKEUP_SOURCE_LP_TIMER,
		.lp_timer_sleep_duration_us = req->lp_period_ms * 1000u,
	};

	if (ulp_lp_core_load_binary(lp_monitor_bin_start,
				    lp_monitor_bin_end - lp_monitor_bin_start) != ESP_OK)
		return false;
	ulp_low_raw = temp_low_raw;
	ulp_high_raw = temp_high_raw;
	if (ulp_lp_core_run(&cfg) != ESP_OK)
		return false;
	/* Keep the SAR/TSENS powered in sleep; IDF turns it off otherwise. */
	esp_sleep_sub_mode_config(ESP_SLEEP_USE_ADC_TSEN_MONITOR_MODE, true);
	esp_sleep_enable_ulp_wakeup();
	return true;
}

/*
 * lp_core=sleep (#59): start the program Linux loaded, as ulp_lp_core_run()
 * would after ulp_lp_core_load_binary().  Linux left the period in the shared
 * block; HP_CPU runs it once now, and it schedules its own next runs.
 */
static bool lp_user_start(void)
{
	uint32_t info = REG_READ(LP_SYSTEM_REG_LP_STORE12_REG);
	ulp_lp_core_cfg_t cfg = {
		.wakeup_source = ULP_LP_CORE_WAKEUP_SOURCE_LP_TIMER |
				 ULP_LP_CORE_WAKEUP_SOURCE_HP_CPU,
		.lp_timer_sleep_duration_us =
			ulp_lp_core_memory_shared_cfg_get()->sleep_duration_us,
	};

	if (info >> 24 != LP_PROGRAM_MAGIC ||
	    ((info >> 22) & 3u) != LP_PROGRAM_MODE_SLEEP)
		return false;
	return ulp_lp_core_run(&cfg) == ESP_OK;
}

/* The LP core's results, read after the wake; it is stopped first. */
static void lp_collect(int32_t base)
{
	ulp_lp_core_stop();
	report.lp_checks = ulp_checks;
	if (ulp_checks) {
		report.lp_last_mc = raw_to_mc(ulp_last_raw, base);
		report.lp_min_mc = raw_to_mc(ulp_min_raw, base);
		report.lp_max_mc = raw_to_mc(ulp_max_raw, base);
	}
	if (ulp_why == 1)
		report.causes |= WAKE_CAUSE_TEMP_LOW;
	else if (ulp_why == 2)
		report.causes |= WAKE_CAUSE_TEMP_HIGH;
}

/* Look at how this boot started; a sleep state in LP RAM means a wake. */
static void classify_boot(uint32_t rom_reason, uint64_t rtc_now)
{
	if (!lp_state_valid()) {
		report.boot = rom_reason == RESET_REASON_CHIP_POWER_ON ?
			      WAKE_BOOT_POWERON : WAKE_BOOT_RESET;
		return;
	}

	/* A copy: lp_state is cleared below. */
	const struct sleep_request saved = lp_state.req;
	const struct sleep_request *req = &saved;
	const int32_t base = lp_state.temp_base_mc;
	const bool lp_user = req->flags & (SLEEP_FLAG_LP_KEEP | SLEEP_FLAG_LP_RUN);
	uint64_t slept_us;
	uint32_t pins;

	rtc_us_at_sleep = ((uint64_t)lp_state.rtc_us_hi << 32) | lp_state.rtc_us_lo;
	slept_us = rtc_now > rtc_us_at_sleep ? rtc_now - rtc_us_at_sleep : 0;
	report.seq = req->seq;
	report.slept_ms = slept_us / 1000u > UINT32_MAX ? UINT32_MAX :
			  (uint32_t)(slept_us / 1000u);
	/* Linux stamps the request just before the reset; this boot then took
	 * boot_us to reach the sleep. */
	if (req->wall_sec_lo || req->wall_sec_hi) {
		wall_known = true;
		wall_us_at_sleep = ((((uint64_t)req->wall_sec_hi << 32) |
				     req->wall_sec_lo) * 1000000u) + req->wall_usec +
				   lp_state.boot_us;
	}
	/* Consume the state first, so a crash below cannot report it twice. */
	memset(&lp_state, 0, sizeof(lp_state));
	if (req->lp_period_ms)
		lp_collect(base);
	/* lp_core=sleep: the program ran for this sleep only. */
	if (req->flags & SLEEP_FLAG_LP_RUN)
		ulp_lp_core_stop();

	if (rom_reason != RESET_REASON_CORE_MWDT &&
	    rom_reason != RESET_REASON_CORE_PMU_PWR_DOWN) {
		report.boot = WAKE_BOOT_INTERRUPTED;
		return;
	}
	report.boot = WAKE_BOOT_DEEPSLEEP;
	pins = (uint32_t)rtc_hal_ext1_get_wakeup_status() & req->gpio_mask;
	rtc_hal_ext1_clear_wakeup_status();
	if (pins) {
		report.causes |= WAKE_CAUSE_GPIO;
		report.gpio_status = pins;
	}
	/* The LP timer runs about 0.14 % short (#56), so allow 1 %.  A user LP
	 * program wakes the chip without a trace, so with one only the time
	 * tells the timer from it. */
	if (req->timer_ms &&
	    ((!lp_user && !(report.causes & ~WAKE_CAUSE_TIMER)) ||
	     report.slept_ms >= req->timer_ms - req->timer_ms / 100u))
		report.causes |= WAKE_CAUSE_TIMER;
	if (lp_user && !report.causes)
		report.causes |= WAKE_CAUSE_LP_CORE;
}

/* Linux's new sleep memory into LP RAM, which survives Deep Sleep. */
static void take_sleep_mem(void)
{
	static struct sleep_mem m;

	psram_read(SLEEP_MEM_PA, &m, sizeof(m));
	if (m.magic != SLEEP_MEM_MAGIC || m.state != SLEEP_MEM_PENDING ||
	    m.len > SLEEP_MEM_BYTES || m.crc32 != crc32_ieee(m.data, m.len))
		return;
	memcpy(lp_mem.data, m.data, m.len);
	lp_mem.len = m.len;
	lp_mem.crc32 = m.crc32;
	lp_mem.magic = LP_MEM_MAGIC;
}

static uint32_t check_request(const struct sleep_request *req, bool button)
{
	uint32_t active = 0;
	bool temp = req->lp_temp_low_mc != SLEEP_TEMP_OFF ||
		    req->lp_temp_high_mc != SLEEP_TEMP_OFF;
	uint32_t lp_user = req->flags & (SLEEP_FLAG_LP_KEEP | SLEEP_FLAG_LP_RUN);

	if (!req->timer_ms && !req->gpio_mask && !temp && !lp_user)
		return SLEEP_SKIP_BAD_REQUEST;
	/* One LP core: a user program or the temperature check. */
	if (lp_user && (temp || lp_user == (SLEEP_FLAG_LP_KEEP | SLEEP_FLAG_LP_RUN)))
		return SLEEP_SKIP_BAD_REQUEST;
	if (req->gpio_mask & ~BOARD_SLEEP_WAKE_GPIO_MASK)
		return SLEEP_SKIP_BAD_REQUEST;
	if (temp != (req->lp_period_ms != 0) ||
	    (temp && (req->lp_period_ms < SLEEP_LP_PERIOD_MIN_MS ||
		      req->lp_period_ms > SLEEP_LP_PERIOD_MAX_MS)))
		return SLEEP_SKIP_BAD_REQUEST;
	if (button)
		return SLEEP_SKIP_BUTTON;
	if (boot_update_pending())
		return SLEEP_SKIP_BOOT_UPDATE;

	for (int pin = 0; pin < 16; ++pin) {
		uint32_t bit = 1u << pin;
		bool high = req->gpio_high & bit;

		if (!(req->gpio_mask & bit))
			continue;
		rtc_gpio_init(pin);
		rtc_gpio_set_direction(pin, RTC_GPIO_MODE_INPUT_ONLY);
		rtc_gpio_pullup_dis(pin);
		rtc_gpio_pulldown_dis(pin);
		if (req->flags & SLEEP_FLAG_PULL) {
			if (high)
				rtc_gpio_pulldown_en(pin);
			else
				rtc_gpio_pullup_en(pin);
		}
	}
	esp_rom_delay_us(2000);
	for (int pin = 0; pin < 16; ++pin) {
		uint32_t bit = 1u << pin;

		if ((req->gpio_mask & bit) &&
		    rtc_gpio_get_level(pin) == !!(req->gpio_high & bit))
			active |= bit;
	}
	if (active) {
		for (int pin = 0; pin < 16; ++pin)
			if (req->gpio_mask & (1u << pin))
				rtc_gpio_deinit(pin);
		report.skipped_gpio = active;
		return SLEEP_SKIP_GPIO_ACTIVE;
	}
	return temp ? temp_prepare(req) : SLEEP_SKIP_NONE;
}

static void enter_sleep(const struct sleep_request *req)
{
	uint32_t high = req->gpio_mask & req->gpio_high;
	uint32_t low = req->gpio_mask & ~req->gpio_high;
	uint64_t rtc_now;

	ets_printf("boot: DEEP_SLEEP seq=%u timer_ms=%u gpio=0x%04x high=0x%04x\n",
		   (unsigned)req->seq, (unsigned)req->timer_ms,
		   (unsigned)req->gpio_mask, (unsigned)high);
	c6_power_off();
	if (req->timer_ms)
		esp_sleep_enable_timer_wakeup((uint64_t)req->timer_ms * 1000u);
	rtc_hal_ext1_clear_wakeup_status();
	if ((req->lp_period_ms && !lp_start(req)) ||
	    ((req->flags & SLEEP_FLAG_LP_RUN) && !lp_user_start())) {
		ets_printf("boot: DEEP_SLEEP LP core did not start; not sleeping\n");
		report.skipped = SLEEP_SKIP_BAD_REQUEST;
		return;
	}
	/* A user program may wake the chip; with LP_KEEP it just runs on. */
	if (req->flags & (SLEEP_FLAG_LP_KEEP | SLEEP_FLAG_LP_RUN))
		esp_sleep_enable_ulp_wakeup();
	if (high)
		esp_sleep_enable_ext1_wakeup_io(high, ESP_EXT1_WAKEUP_ANY_HIGH);
	if (low)
		esp_sleep_enable_ext1_wakeup_io(low, ESP_EXT1_WAKEUP_ANY_LOW);

	rtc_now = esp_rtc_get_time_us();
	lp_state.req = *req;
	lp_state.rtc_us_lo = (uint32_t)rtc_now;
	lp_state.rtc_us_hi = (uint32_t)(rtc_now >> 32);
	lp_state.boot_us = (uint32_t)esp_timer_get_time();
	lp_state.temp_base_mc = temp_base_mc;
	lp_state.magic = LP_STATE_MAGIC;
	lp_state.crc32 = crc32_ieee(&lp_state, offsetof(struct lp_sleep_state, crc32));
	esp_deep_sleep_start();
}

void deep_sleep_early(void)
{
	uint32_t rom_reason = esp_rom_get_reset_reason(0);
	uint64_t rtc_now = esp_rtc_get_time_us();
	struct sleep_request req;

	memset(&report, 0, sizeof(report));
	classify_boot(rom_reason, rtc_now);
	report.button = button_held();
	if (!lp_mem_valid())
		memset(&lp_mem, 0, sizeof(lp_mem));
	take_sleep_mem();

	psram_read(SLEEP_REQUEST_PA, &req, sizeof(req));
	if (req.magic != SLEEP_REQUEST_MAGIC || req.version != SLEEP_REQUEST_VERSION ||
	    req.commit_word != SLEEP_REQUEST_COMMIT ||
	    req.record_crc32 != crc32_ieee(&req, offsetof(struct sleep_request,
							   record_crc32))) {
		ets_printf("boot: WAKE boot=%u causes=0x%x gpio=0x%04x button=%u slept_ms=%u\n",
			   (unsigned)report.boot, (unsigned)report.causes,
			   (unsigned)report.gpio_status, (unsigned)report.button,
			   (unsigned)report.slept_ms);
		return;
	}
	/* One-shot: clear the commit before acting on it. */
	{
		uint32_t zero = 0;

		psram_write(SLEEP_REQUEST_PA + offsetof(struct sleep_request, commit_word),
			    &zero, sizeof(zero));
	}
	report.skipped = check_request(&req, report.button);
	/* Returns only when the LP core did not start; it sets skipped then. */
	if (report.skipped == SLEEP_SKIP_NONE)
		enter_sleep(&req);
	report.seq = req.seq;
	ets_printf("boot: DEEP_SLEEP skipped seq=%u reason=%u gpio=0x%04x\n",
		   (unsigned)req.seq, (unsigned)report.skipped,
		   (unsigned)report.skipped_gpio);
}

void deep_sleep_publish(void)
{
	static struct sleep_mem m;

	report.magic = WAKE_INFO_MAGIC;
	report.version = WAKE_INFO_VERSION;
	report.lp_ram_base = (uint32_t)(uintptr_t)&_rtc_ulp_memory_start;
	report.lp_ram_bytes = CONFIG_ULP_COPROC_RESERVE_MEM;
	if (wall_known) {
		uint64_t now = esp_rtc_get_time_us();
		uint64_t wall = wall_us_at_sleep +
				(now > rtc_us_at_sleep ? now - rtc_us_at_sleep : 0);
		uint64_t sec = wall / 1000000u;

		report.wall_sec_lo = (uint32_t)sec;
		report.wall_sec_hi = (uint32_t)(sec >> 32);
		report.wall_usec = (uint32_t)(wall % 1000000u);
	}
	report.record_crc32 = crc32_ieee(&report, offsetof(struct wake_info, record_crc32));
	psram_write(WAKE_INFO_PA, &report, sizeof(report));

	memset(&m, 0, sizeof(m));
	m.magic = SLEEP_MEM_MAGIC;
	m.state = SLEEP_MEM_PUBLISHED;
	if (lp_mem_valid()) {
		m.len = lp_mem.len;
		memcpy(m.data, lp_mem.data, lp_mem.len);
	}
	m.crc32 = crc32_ieee(m.data, m.len);
	psram_write(SLEEP_MEM_PA, &m, sizeof(m));
}
