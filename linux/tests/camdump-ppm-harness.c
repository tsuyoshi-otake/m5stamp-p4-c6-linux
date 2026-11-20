// SPDX-License-Identifier: GPL-2.0-only
/*
 * camdump-ppm-harness: host-side driver for cam-dump's BGGR -> PPM
 * converter (#44) so linux/tests/test-cam-dump-ppm.sh can feed
 * synthetic frames without a board.  Linked with the same
 * bggr-ppm.c the package builds.
 *
 *   fill R G B              -> 1280x720 BGGR frame on stdout, every
 *                              site at its channel's value
 *   block ROW COL B G1 G2 R -> all-zero frame except the 2x2 block
 *                              behind output pixel (ROW,COL)
 *   ppm [-W]                -> raw frame on stdin, P6 PPM on stdout,
 *                              the wb line cam-dump prints on stderr
 *   px ROW COL              -> "R G B" of that output pixel (PPM on
 *                              stdin)
 */
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "bggr-ppm.h"

#define FBYTES	(CDP_SRC_W * CDP_SRC_H)

static uint8_t fr[FBYTES];

static int fill(int r, int g, int b)
{
	size_t x, y;

	for (y = 0; y < CDP_SRC_H; y++)
		for (x = 0; x < CDP_SRC_W; x++)
			fr[y * CDP_SRC_W + x] = (uint8_t)(y & 1
				? (x & 1 ? r : g)
				: (x & 1 ? g : b));
	return fwrite(fr, 1, FBYTES, stdout) == FBYTES ? 0 : 1;
}

static int block(unsigned orow, unsigned ocol, int b, int g1, int g2, int r)
{
	size_t y = orow * 2, x = ocol * 2;

	if (orow >= CDP_OUT_H || ocol >= CDP_OUT_W)
		return 2;
	fr[y * CDP_SRC_W + x] = (uint8_t)b;
	fr[y * CDP_SRC_W + x + 1] = (uint8_t)g1;
	fr[(y + 1) * CDP_SRC_W + x] = (uint8_t)g2;
	fr[(y + 1) * CDP_SRC_W + x + 1] = (uint8_t)r;
	return fwrite(fr, 1, FBYTES, stdout) == FBYTES ? 0 : 1;
}

/* Mirror of cam-dump's write_ppm(): same calls, same stderr line. */
static int ppm(int wb)
{
	uint8_t row[CDP_OUT_W * 3];
	unsigned mr, mg, mb;
	uint32_t gr, gb;
	size_t r;

	if (fread(fr, 1, FBYTES, stdin) != FBYTES) {
		fprintf(stderr, "short input frame\n");
		return 1;
	}
	cdp_bggr_means(fr, CDP_SRC_W, CDP_SRC_H, &mr, &mg, &mb);
	cdp_wb_gains(mr, mg, mb, wb, &gr, &gb);
	if (wb)
		fprintf(stderr,
			"wb: meanR=%u meanG=%u meanB=%u gainR=%u.%02u gainB=%u.%02u\n",
			mr, mg, mb,
			gr >> 8, ((gr & 0xff) * 100) >> 8,
			gb >> 8, ((gb & 0xff) * 100) >> 8);
	else
		fprintf(stderr, "wb: off (meanR=%u meanG=%u meanB=%u)\n",
			mr, mg, mb);
	if (printf("P6\n%d %d\n255\n", CDP_OUT_W, CDP_OUT_H) < 0)
		return 1;
	for (r = 0; r < CDP_OUT_H; r++) {
		cdp_bggr_row(fr, CDP_SRC_W, r, gr, gb, row);
		if (fwrite(row, 1, sizeof row, stdout) != sizeof row)
			return 1;
	}
	return 0;
}

static int px(unsigned orow, unsigned ocol)
{
	static uint8_t img[16 + CDP_OUT_W * CDP_OUT_H * 3];
	size_t n = fread(img, 1, sizeof img, stdin), pos = 0, off;
	int nl = 0;

	while (pos < n && nl < 3)
		if (img[pos++] == '\n')
			nl++;
	if (nl < 3 || orow >= CDP_OUT_H || ocol >= CDP_OUT_W)
		return 2;
	off = pos + (orow * CDP_OUT_W + ocol) * 3;
	if (off + 2 >= n)
		return 2;
	printf("%u %u %u\n", img[off], img[off + 1], img[off + 2]);
	return 0;
}

int main(int argc, char **argv)
{
	if (argc == 5 && !strcmp(argv[1], "fill"))
		return fill(atoi(argv[2]), atoi(argv[3]), atoi(argv[4]));
	if (argc == 8 && !strcmp(argv[1], "block"))
		return block((unsigned)atoi(argv[2]), (unsigned)atoi(argv[3]),
			     atoi(argv[4]), atoi(argv[5]), atoi(argv[6]),
			     atoi(argv[7]));
	if ((argc == 2 || argc == 3) && !strcmp(argv[1], "ppm") &&
	    (argc == 2 || !strcmp(argv[2], "-W")))
		return ppm(argc == 2);
	if (argc == 4 && !strcmp(argv[1], "px"))
		return px((unsigned)atoi(argv[2]), (unsigned)atoi(argv[3]));
	fprintf(stderr,
		"usage: harness fill R G B | block ROW COL B G1 G2 R | ppm [-W] | px ROW COL\n");
	return 2;
}
