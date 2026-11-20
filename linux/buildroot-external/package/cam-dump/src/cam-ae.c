// SPDX-License-Identifier: GPL-2.0
#include "cam-ae.h"

void cdp_gain_regs(unsigned idx,
		   unsigned *dfine, unsigned *dcoarse, unsigned *ang)
{
	unsigned block, step;

	if (idx > CDP_GAIN_MAX)
		idx = CDP_GAIN_MAX;
	block = idx / 32;
	step = idx % 32;
	*dfine = 0x80 + 4 * step;
	*dcoarse = block ? 1 : 0;
	*ang = block ? (1u << (block - 1)) - 1 : 0;
}

int cdp_ae_step(unsigned mean, unsigned target,
		unsigned *exp, unsigned *gain)
{
	unsigned long scale, resid, r;
	unsigned ne;
	int ng, steps;

	if (target == 0)
		return 0;
	if (mean >= target - target / 8 && mean <= target + target / 8)
		return 0;
	/* Q16 scale toward the target, at most x4 or /4 per step so a
	 * few iterations converge without overshooting the range. */
	scale = mean ? ((unsigned long)target << 16) / mean
		     : 4UL << 16;
	if (scale < (1UL << 14))
		scale = 1UL << 14;
	if (scale > (4UL << 16))
		scale = 4UL << 16;

	ne = (unsigned)(((*exp * scale) + 0x8000) >> 16);
	if (ne < CDP_EXP_MIN)
		ne = CDP_EXP_MIN;
	if (ne > CDP_EXP_MAX)
		ne = CDP_EXP_MAX;

	/* The part of the scale exposure could not take (Q16):
	 * scale*exp/ne.  ~1.0 when the move was free, >1 clamped at
	 * the top, <1 at the bottom -- the leftover moves the gain
	 * index, ~32 steps per doubling. */
	resid = *exp ? (scale * *exp) / ne : scale;
	steps = 0;
	r = resid;
	while (r >= (2UL << 16)) { steps += 32; r >>= 1; }
	while (r < (1UL << 16)) { steps -= 32; r <<= 1; }
	steps += (int)(((r - (1UL << 16)) * 32) >> 16);
	ng = (int)*gain + steps;
	if (ng < 0)
		ng = 0;
	if (ng > CDP_GAIN_MAX)
		ng = CDP_GAIN_MAX;
	if (ne == *exp && (unsigned)ng == *gain)
		return 0;
	*exp = ne;
	*gain = (unsigned)ng;
	return 1;
}
