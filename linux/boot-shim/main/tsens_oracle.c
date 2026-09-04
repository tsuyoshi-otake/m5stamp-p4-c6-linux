/* SPDX-License-Identifier: Apache-2.0 OR GPL-2.0-only */
/*
 * ESP-IDF-side ESP32-P4 LP-TSENS reference measurement.
 *
 * This file is compiled only for an explicit oracle build.  It intentionally
 * uses the same low-level operations as ESP-IDF v5.5.3:
 *
 *   - enable the SAR/TSENS internal REGI2C path;
 *   - enable and reset LP-TSENS;
 *   - program the offset-0 range (REGI2C DAC value 15);
 *   - enable sampling and wait for the documented settling interval.
 *
 * The result is an independent reference for a future Linux driver.  The
 * boot-shim must not leave Linux dependent on this configuration: the sensor,
 * its clock, and its internal REGI2C power path are disabled before return.
 */

#include "tsens_oracle.h"

#include <inttypes.h>
#include <stdint.h>
#include <stdbool.h>

#include "esp_rom_sys.h"
#include "esp32p4/rom/ets_sys.h"
#include "hal/regi2c_ctrl.h"
#include "hal/regi2c_ctrl_ll.h"
#include "soc/efuse_struct.h"
#include "soc/lpperi_struct.h"
#include "soc/regi2c_saradc.h"
#include "soc/soc.h"
#include "soc/tsens_struct.h"

#define TSENS_ORACLE_DAC_PROBE       7u
#define TSENS_ORACLE_DAC_REGVAL       15u
#define TSENS_ORACLE_OFFSET           0
#define TSENS_ORACLE_SAMPLES          8u
#define TSENS_ORACLE_SETTLE_US        300u
#define TSENS_ORACLE_SAMPLE_GAP_US    100u

/*
 * ESP-IDF v5.5.3:
 *   TEMPERATURE_SENSOR_LL_ADC_FACTOR    = 0.4386
 *   TEMPERATURE_SENSOR_LL_DAC_FACTOR    = 27.88
 *   TEMPERATURE_SENSOR_LL_OFFSET_FACTOR = 20.52
 *
 * The integer result is milli-degrees Celsius.  Thus 0.4386 C/sample is
 * 438.6 mC/sample, represented as 4386/10.  This denominator is deliberately
 * not 10000: the numerator is already expressed in milli-degrees.
 */
#define TSENS_ADC_MILLI_NUM        4386
#define TSENS_ADC_MILLI_DEN        10
#define TSENS_DAC_MILLI             27880
#define TSENS_OFFSET_MILLI          20520

static int tsens_decode_delta_t(uint32_t efuse_raw)
{
	/*
	 * ESP-IDF's P4 decoder is not two's-complement:
	 * bit 8 is the sign and bits 7:0 are the magnitude.  Bit 9 is unused.
	 */
	uint32_t magnitude = efuse_raw & 0xffu;

	return (efuse_raw & BIT(8)) ? -(int)magnitude : (int)magnitude;
}

static int32_t tsens_raw_to_mdeg(uint32_t raw, int delta_t)
{
	int64_t temp_mc;

	/* offset=0 is a deliberate Phase-0 fixed-range choice. */
	temp_mc = ((int64_t)raw * TSENS_ADC_MILLI_NUM +
		   TSENS_ADC_MILLI_DEN / 2) / TSENS_ADC_MILLI_DEN;
	temp_mc -= TSENS_OFFSET_MILLI;
	temp_mc -= (int64_t)delta_t * 100;
	return (int32_t)temp_mc;
}

static void tsens_oracle_shutdown(void)
{
	LP_TSENS.ctrl.sample_en = 0;
	LP_TSENS.wakeup_ctrl.wakeup_en = 0;
	LP_TSENS.ctrl.power_up = 0;
	LPPERI.clk_en.ck_en_lp_tsens = 0;
	regi2c_ctrl_ll_i2c_sar_periph_disable();
}

static bool tsens_oracle_fail(const char *stage)
{
	tsens_oracle_shutdown();
	ets_printf("P4_TSENS_REF FAIL stage=%s\n", stage);
	return false;
}

bool easystick_tsens_oracle_run(void)
{
	uint32_t efuse_raw;
	int delta_t;
	uint32_t dac_before;
	uint32_t dac_probe;
	uint32_t dac_after;
	uint32_t raw_min = UINT32_MAX;
	uint32_t raw_max = 0;
	uint32_t raw_sum = 0;
	uint32_t raw_avg;
	uint32_t ready = 0;
	int32_t temp_mc;

	efuse_raw = EFUSE.rd_sys_part2_data3.temperature_sensor & 0x3ffu;
	delta_t = tsens_decode_delta_t(efuse_raw);

	/*
	 * This is the P4 equivalent of ESP-IDF's regi2c_saradc_enable().  The
	 * shim runs before Linux and no other SAR/TSENS consumer exists, so the
	 * LL path is sufficient and its readback is a hard gate below.
	 */
	regi2c_ctrl_ll_i2c_sar_periph_enable();

	LPPERI.clk_en.ck_en_lp_tsens = 1;
	LPPERI.reset_en.rst_en_lp_tsens = 1;
	LPPERI.reset_en.rst_en_lp_tsens = 0;

	dac_before = REGI2C_READ_MASK(I2C_SAR_ADC, I2C_SARADC_TSENS_DAC);
	/*
	 * Prove that the write path is live instead of accepting a power-on
	 * default of 15 as evidence that the required write happened.
	 */
	REGI2C_WRITE_MASK(I2C_SAR_ADC, I2C_SARADC_TSENS_DAC,
			  TSENS_ORACLE_DAC_PROBE);
	dac_probe = REGI2C_READ_MASK(I2C_SAR_ADC, I2C_SARADC_TSENS_DAC);
	if (dac_probe != TSENS_ORACLE_DAC_PROBE)
		return tsens_oracle_fail("dac-write-probe");
	REGI2C_WRITE_MASK(I2C_SAR_ADC, I2C_SARADC_TSENS_DAC,
			  TSENS_ORACLE_DAC_REGVAL);
	dac_after = REGI2C_READ_MASK(I2C_SAR_ADC, I2C_SARADC_TSENS_DAC);
	if (dac_after != TSENS_ORACLE_DAC_REGVAL)
		return tsens_oracle_fail("dac-readback");

	LP_TSENS.ctrl.power_up = 1;
	LP_TSENS.wakeup_ctrl.wakeup_en = 1;
	LP_TSENS.ctrl.sample_en = 1;
	esp_rom_delay_us(TSENS_ORACLE_SETTLE_US);

	for (uint32_t i = 0; i < TSENS_ORACLE_SAMPLES; ++i) {
		uint32_t raw = LP_TSENS.ctrl.out;

		if (raw < raw_min)
			raw_min = raw;
		if (raw > raw_max)
			raw_max = raw;
		raw_sum += raw;
		ready |= LP_TSENS.ctrl.ready;
		if (i + 1u < TSENS_ORACLE_SAMPLES)
			esp_rom_delay_us(TSENS_ORACLE_SAMPLE_GAP_US);
	}

	raw_avg = (raw_sum + TSENS_ORACLE_SAMPLES / 2u) /
		  TSENS_ORACLE_SAMPLES;
	temp_mc = tsens_raw_to_mdeg(raw_avg, delta_t);
	if (temp_mc < -40000 || temp_mc > 125000)
		return tsens_oracle_fail("temperature-out-of-range");

	/*
	 * ESP-IDF v5.5.3 reads LP_TSENS.ctrl.out directly and does not gate
	 * temperature_sensor_get_celsius() on ctrl.ready.  On this P4 board the
	 * raw output is live (and varies) while ready remains zero; treat READY as
	 * diagnostic telemetry rather than inventing a stronger contract than the
	 * reference driver has.
	 */
	ets_printf(
		"P4_TSENS_REF PASS efuse_raw=0x%03" PRIx32
		" efuse_sign=%u delta_t=%d range_offset=%d range_reg=%u"
		" dac_before=%" PRIu32 " dac_probe=%" PRIu32
		" dac_readback=%" PRIu32
		" ready=%" PRIu32 " raw_min=%" PRIu32 " raw_max=%" PRIu32
		" raw_avg=%" PRIu32 " temp_mc=%" PRId32 "\n",
		efuse_raw, (efuse_raw & BIT(8)) ? 1u : 0u, delta_t,
		TSENS_ORACLE_OFFSET, TSENS_ORACLE_DAC_REGVAL, dac_before,
		dac_probe, dac_after, ready, raw_min, raw_max, raw_avg, temp_mc);

	tsens_oracle_shutdown();
	ets_printf("P4_TSENS_REF CLEANUP sensor=off clock=off regi2c=off\n");
	return true;
}
