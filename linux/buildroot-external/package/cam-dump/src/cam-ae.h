/* SPDX-License-Identifier: GPL-2.0
 *
 * cam-ae — exposure/gain helpers for the SC202CS (Tab5 cam-dump).
 *
 * Register facts mirror the esp_cam_sensor vendor driver (issue #44):
 *   shutter  0x3e00[3:0]/0x3e01/0x3e02[7:4]  = a 16-bit "exposure
 *            value" field whose low nibble is a fraction of a line
 *            (sc202cs.c FETCH_EXP_* + "4 least significant bits are
 *            fractional"); clamp [0x08, vts-6], vts=1250 at 720p30.
 *   gain     index 0..191 of the DIG-priority map -> 0x3e07 fine,
 *            0x3e06 coarse, 0x3e09 analog; computed by cdp_gain_regs
 *            instead of embedding the 192-entry table.
 */
#ifndef CAM_AE_H
#define CAM_AE_H

#define CDP_EXP_DEF	0x3dc		/* sensor table + vendor exp_def */
#define CDP_EXP_MIN	0x08		/* s_sc202cs_exp_min */
#define CDP_EXP_MAX	(1250 - 6)	/* vts-6, 720p30 format */
#define CDP_GAIN_MAX	191		/* last DIG-priority map index */

/* Vendor shutter-field split (sc202cs.c FETCH_EXP_{H,M,L}). */
#define CDP_EXP_H(v)	(((v) >> 12) & 0x0f)
#define CDP_EXP_M(v)	(((v) >> 4) & 0xff)
#define CDP_EXP_L(v)	(((v) & 0x0f) << 4)

/* DIG-priority gain index -> {0x3e07, 0x3e06, 0x3e09}.  The map is
 * six 32-entry blocks: fine sweeps 0x80..0xfc, coarse 0/1, analog
 * doubles from block 2.  idx is clamped to CDP_GAIN_MAX. */
void cdp_gain_regs(unsigned idx,
		   unsigned *dfine, unsigned *dcoarse, unsigned *ang);

/* One auto-exposure step toward target mean.  Exposure moves first;
 * the part of the scale it could not take (clamped at min/max) moves
 * the gain index instead (~32 index steps per x2).  Returns 1 when
 * exp/gain changed, 0 when the mean is inside the target deadband
 * (+-1/8) or nothing could move. */
int cdp_ae_step(unsigned mean, unsigned target,
		unsigned *exp, unsigned *gain);

#endif
