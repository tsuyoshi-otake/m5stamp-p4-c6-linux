// SPDX-License-Identifier: Apache-2.0 OR GPL-2.0-only
/*
 * AUDIO_ORACLE: build-time diagnostic for the Tab5 audio path (#24).
 * Never enabled in release shims -- CMake only compiles this file when
 * the environment sets AUDIO_ORACLE=1.
 */
#pragma once

#include <stdbool.h>

/*
 * Play a 1 kHz tone through the ES8388 speaker path using IDF's own
 * i2s_std channel (the same driver the stock demo uses), and while it
 * plays dump the I2S0 register window, the AHB-GDMA engine state, the
 * hp_sys_clkrst / lp_clkrst bits the Linux I2S driver writes, the GPIO
 * matrix routes for the five I2S pins, and the first words of the live
 * DMA descriptors.  Every register prints as one "R 0xADDR = 0xVAL"
 * line so the dump diffs cleanly against devmem reads taken from Linux
 * while pcm-sine runs.  Returns false on any step failure (the boot
 * continues -- this is a diagnostic, not a contract).
 */
bool audio_oracle_run(void);
