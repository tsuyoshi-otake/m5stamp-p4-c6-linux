// SPDX-License-Identifier: Apache-2.0 OR GPL-2.0-only
/* #25 CAMERA_ORACLE (diagnostic builds only, never in release). */
#pragma once

#include <stdbool.h>
#include <stdint.h>

/* Runs the SC2356 SCCB init + one-frame esp_driver_cam CSI capture,
 * prints frame stats and the CSI/bridge/GDMA/CLKRST register dump,
 * then renders a grey downscale into the panel framebuffer (fb =
 * s_tab5_fb.pa, identity-mapped PSRAM; NULL skips the drawing) and
 * holds it for a few seconds.  Returns true when a real frame was
 * captured. */
bool camera_oracle_run(uint16_t *fb, uint32_t fb_bytes);
