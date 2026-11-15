// SPDX-License-Identifier: Apache-2.0 OR GPL-2.0-only
/*
 * M5Stack Tab5 board bootstrap, called by the boot shim only when the shim
 * is built with ES_BOARD=tab5 (CMake: BOARD_TAB5).  See
 * board-contract.json for the hardware sources behind every GPIO and
 * expander bit used here.
 */
#pragma once

#include <stdint.h>
#include <stdbool.h>

/* Framebuffer handoff for patch_display_window() in main.c. */
struct tab5_fb {
	uint32_t pa;
	uint32_t bytes;
	bool ok;
};

/*
 * Program the two PI4IOE5V6408 expanders to a safe boot state
 * (speaker/touch/camera/USB off, C6 rail on), route SDMMC slot 1 through
 * the GPIO matrix onto Tab5's CLK=12 CMD=13 D0-3={11,10,9,8}, pulse C6
 * reset on GPIO15, and keep the backlight off.  Never returns on a
 * contract violation that would risk the board; logs and continues past
 * non-fatal failures (e.g. display) so the serial recovery path stays.
 */
void tab5_board_init(struct tab5_fb *fb);

/* #56 deep-sleep hook: clear expander 0x44 P0 (the C6 rail) over the
 * internal I2C bus; the Tab5 equivalent of the Stamp GPIO42 latch. */
void tab5_c6_power_off(void);

/*
 * Display+touch chip fitted to this Tab5.  Units ship with one of two
 * MIPI-DSI panels that need different init tables and video timings, and
 * the stock BSP (bsp_detect_display_type in m5stack_tab5.c) tells them
 * apart by the touch controller on I2C 0x55: its 16-bit register 0x0000
 * reads 1 on an ST7121 and 3 on an ST7123.
 */
enum tab5_panel {
	TAB5_PANEL_NONE = 0, /* nothing answers at 0x55: not an ST712x unit */
	TAB5_PANEL_ST7121,
	TAB5_PANEL_ST7123,
};

/*
 * Pulse LCD_RST (0x43 P4) and TP_RST (0x43 P5) together for 100 ms and
 * wait 100 ms after release, exactly like the stock bsp_reset_tp() that
 * runs right before the stock display init.  The expanders must be up.
 */
void tab5_panel_reset(void);

/* Read the touch controller's firmware byte and say which panel it is.
 * Unknown firmware on a controller that does answer is an ST7123, as in
 * the BSP.  *fw gets the raw byte (0 if nothing answered). */
enum tab5_panel tab5_detect_panel(uint8_t *fw);

/*
 * Pre-jump handoff for the internal I2C bus: delete the IDF master bus
 * (returns SDA/SCL to plain GPIO, frees the I2C interrupt), detach the
 * I2C0 source at the interrupt matrix, disarm its old CLIC slot, and
 * leave GPIO23 as a pull-up'd input.  Safe to call when the bus never
 * came up.  The touch controller itself is not reset -- expander 0x43
 * P5 (TP_RST) stays released so Linux finds it running at 0x55.
 */
void tab5_i2c_release(void);
