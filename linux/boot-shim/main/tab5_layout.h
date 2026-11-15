// SPDX-License-Identifier: Apache-2.0 OR GPL-2.0-only
/*
 * Single source of truth for the PSRAM windows that the boot shim and
 * the Tab5 display code must never let overlap: the Linux load
 * addresses (also used by main.c) and the reserved-memory regions that
 * esp32p4-tab5.dts hands to Linux.  tests/static-check.sh compares
 * these values against the DTS so a layout change on either side fails
 * the check instead of silently overlapping the framebuffer.
 */
#pragma once

#include <stdint.h>

#define TAB5_KERNEL_LOAD_PA 0x48000000u /* matches KERNEL_LOAD_PA in main.c */
#define TAB5_DTB_LOAD_PA    0x48800000u /* matches DTB_LOAD_PA in main.c */
#define TAB5_LOAD_RESERVE_BYTES (8u * 1024u * 1024u)

/* PSRAM regions the shim fills or Linux reserves.  The address/size
 * pairs must stay identical to the reserved-memory nodes in
 * esp32p4-tab5.dts (static-check enforces). */
static const uint32_t tab5_fb_guard_regions[][2] = {
	{ 0x48000000u, 0x00800000u }, /* kernel Image window */
	{ 0x48800000u, 0x00800000u }, /* DTB window */
	{ 0x495af000u, 0x00400000u }, /* nommu-userspace-pool low */
	{ 0x499bf000u, 0x00001000u }, /* boot-update-request */
	{ 0x499c0000u, 0x00040000u }, /* boot-partition staging */
	{ 0x49a00000u, 0x00600000u }, /* nommu-userspace-pool */
};
