/* SPDX-License-Identifier: Apache-2.0 OR GPL-2.0-only */
#pragma once

#include <stdbool.h>

/*
 * Run the one-shot ESP-IDF-side LP-TSENS reference measurement.
 *
 * This is an oracle build feature, not part of the normal boot path.  The
 * caller must treat false as fatal: a value is not useful as a reference
 * unless the analog DAC write/readback and the sensor sample both succeed.
 */
bool easystick_tsens_oracle_run(void);
