// SPDX-License-Identifier: GPL-2.0
/* See bggr-ppm.h -- BGGR 2x2 binning + gray-world white balance
 * for `cam-dump -f ppm` (#44). */

#include "bggr-ppm.h"

void cdp_bggr_means(const uint8_t *f, size_t w, size_t h,
		    unsigned *mr, unsigned *mg, unsigned *mb)
{
	uint64_t sr = 0, sg = 0, sb = 0;
	size_t x, y, n = 0;

	for (y = 0; y < h; y += 2) {
		const uint8_t *t = f + y * w, *b = t + w;

		for (x = 0; x < w; x += 2) {
			sb += t[x];
			sg += t[x + 1] + b[x];
			sr += b[x + 1];
			n++;
		}
	}
	*mr = n ? (unsigned)(sr / n) : 0;
	*mg = n ? (unsigned)(sg / (2 * n)) : 0;
	*mb = n ? (unsigned)(sb / n) : 0;
}

void cdp_wb_gains(unsigned mr, unsigned mg, unsigned mb, int wb,
		  uint32_t *gr, uint32_t *gb)
{
	uint32_t r, b;

	if (!wb) {
		*gr = *gb = 256;
		return;
	}
	/* rounded division, then clamp to the 0.25..4.0 Q8 range;
	 * a zero channel mean is the open end of the ratio and
	 * lands on the matching clamp edge. */
	r = mr ? (mg * 256u + mr / 2) / mr : 1024;
	b = mb ? (mg * 256u + mb / 2) / mb : 1024;
	*gr = r < 64 ? 64 : r > 1024 ? 1024 : r;
	*gb = b < 64 ? 64 : b > 1024 ? 1024 : b;
}

void cdp_bggr_row(const uint8_t *f, size_t w, size_t orow,
		  uint32_t gr, uint32_t gb, uint8_t *out)
{
	const uint8_t *t = f + orow * 2 * w, *b = t + w;
	size_t x;

	for (x = 0; x < w; x += 2) {
		unsigned rv = (b[x + 1] * gr) >> 8;
		unsigned bv = (t[x] * gb) >> 8;

		out[0] = rv > 255 ? 255 : (uint8_t)rv;
		out[1] = (uint8_t)((t[x + 1] + b[x]) / 2);
		out[2] = bv > 255 ? 255 : (uint8_t)bv;
		out += 3;
	}
}
