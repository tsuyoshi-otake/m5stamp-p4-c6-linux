// SPDX-License-Identifier: Apache-2.0 OR GPL-2.0-only
#pragma once

#include "tab5_board.h"

/*
 * Bring up the MIPI-DSI panel exactly as the stock BSP does: reset the
 * display+touch chip, tell ST7121 from ST7123 by the touch firmware
 * byte, then LDO_VO3 2.5 V, 2 lanes at 965 Mbps, DPI RGB565 720x1280
 * @ 70 MHz and that chip's init table and vsync timing.  Fill the PSRAM
 * framebuffer with a boot test card, run the DW-GDMA scanout handoff
 * self-test, enable the LEDC
 * backlight, and leave the DPI/GDMA stream running across the Linux
 * jump.
 *
 * On success *fb carries the framebuffer physical address and size so
 * main.c can patch the DTB simple-framebuffer and reserved-memory nodes.
 * On any failure *fb->ok stays false and Linux boots headless.
 */
void tab5_display_init(struct tab5_fb *fb);

/* Clean handoff for main.c to run immediately before the jump:
 * detaches the two display interrupt sources, quiesces the channel
 * once it stalls, clears all display interrupt state, and prints the
 * read-back line. */
void tab5_display_status(void);

/* Scan-out facts for the DTB dsi-scanout node patch: the SRAM LLI
 * address (0 when unknown) and the DW-GDMA channel index (-1 when
 * unknown). */
uint32_t tab5_scanout_lli(void);
int tab5_scanout_chan(void);
