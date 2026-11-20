/* SPDX-License-Identifier: GPL-2.0 */
/*
 * BGGR RAW8 -> 2x2-binned RGB for `cam-dump -f ppm` (#44).
 *
 * Pure functions: no I/O, no allocation.  cam-dump streams the
 * rows to the output fd itself; the host test in linux/tests
 * links the same object and feeds it synthetic frames.
 */

#ifndef BGGR_PPM_H
#define BGGR_PPM_H

#include <stddef.h>
#include <stdint.h>

#define CDP_SRC_W	1280
#define CDP_SRC_H	720
#define CDP_OUT_W	(CDP_SRC_W / 2)	/* 640 */
#define CDP_OUT_H	(CDP_SRC_H / 2)	/* 360 */

/* Per-channel means over a BGGR frame: each 2x2 block is
 * [B G; G R], so R and B sites each cover w*h/4 pixels and G
 * sites w*h/2.  w and h must be even. */
void cdp_bggr_means(const uint8_t *f, size_t w, size_t h,
		    unsigned *mr, unsigned *mg, unsigned *mb);

/* Gray-world gains in Q8 (value/256): gR = meanG/meanR and
 * gB = meanG/meanB, each clamped to 0.25..4.0; G gain is 1.0 by
 * construction so it is not returned.  A zero mean takes the
 * matching clamp edge (meanR/meanB == 0 -> 4.0, meanG == 0 ->
 * 0.25).  wb == 0 returns 1.0 for both gains. */
void cdp_wb_gains(unsigned mr, unsigned mg, unsigned mb, int wb,
		  uint32_t *gr, uint32_t *gb);

/* Convert output row `orow` (0 .. h/2-1) of a BGGR frame into
 * w/2 RGB pixels: B = f[2r][2c], G = (f[2r][2c+1] + f[2r+1][2c])/2,
 * R = f[2r+1][2c+1]; R and B are scaled by their Q8 gains and
 * clamped to 255, G passes through.  `out` must hold w/2*3 bytes. */
void cdp_bggr_row(const uint8_t *f, size_t w, size_t orow,
		  uint32_t gr, uint32_t gb, uint8_t *out);

#endif
