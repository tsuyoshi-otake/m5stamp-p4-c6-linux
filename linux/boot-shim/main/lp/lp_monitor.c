// SPDX-License-Identifier: Apache-2.0 OR GPL-2.0-only
/*
 * LP core program for Deep Sleep (#57).  The LP timer starts it every
 * check period while the HP side sleeps.  It reads the LP temperature sensor
 * and wakes the HP side when the reading stays past a threshold for two
 * checks in a row.  deep_sleep.c sets the thresholds (raw sensor steps) and
 * reads the counters back after the wake; its ulp_* names are these.
 *
 * deep_sleep.c keeps the sensor powered in sleep
 * (ESP_SLEEP_USE_ADC_TSEN_MONITOR_MODE); without that the register holds
 * the last value from before the sleep.
 */
#include <stdint.h>
#include "ulp_lp_core_utils.h"
#include "soc/tsens_struct.h"

/* Set by the HP side before the sleep; 0 turns a threshold off. */
volatile uint32_t low_raw;      /* wake when raw <= low_raw */
volatile uint32_t high_raw;     /* wake when raw >= high_raw */

/* Read by the HP side after the wake. */
volatile uint32_t checks;       /* readings taken (0 reads not counted) */
volatile uint32_t zero_reads;   /* 0 means the sensor had no reading yet */
volatile uint32_t last_raw;
volatile uint32_t min_raw = 0xffffffffu;
volatile uint32_t max_raw;
volatile uint32_t why;          /* 1 low, 2 high; set once */

static uint32_t streak_low, streak_high;

int main(void)
{
	uint32_t raw = LP_TSENS.ctrl.out;

	if (raw == 0) {
		zero_reads++;
		return 0;
	}
	checks++;
	last_raw = raw;
	if (raw < min_raw)
		min_raw = raw;
	if (raw > max_raw)
		max_raw = raw;

	streak_low = low_raw && raw <= low_raw ? streak_low + 1 : 0;
	streak_high = high_raw && raw >= high_raw ? streak_high + 1 : 0;
	if (!why) {
		if (streak_low >= 2)
			why = 1;
		else if (streak_high >= 2)
			why = 2;
	}
	if (why)
		ulp_lp_core_wakeup_main_processor();
	return 0;
}
