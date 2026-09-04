// SPDX-License-Identifier: Apache-2.0 OR GPL-2.0-only

#pragma once

/* Bring up the Stamp-AddOn C6 SDIO Slot 1 pins before Linux takes over.
 * This is deliberately a separate M2 opt-in; M1 does not touch these pins. */
void easystick_m2_sdmmc_init(void);
