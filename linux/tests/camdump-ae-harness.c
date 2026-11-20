// SPDX-License-Identifier: GPL-2.0-only
/*
 * camdump-ae-harness: host-side driver for cam-dump's SC202CS
 * exposure/gain helpers (#44) so linux/tests/test-cam-dump-ae.sh can
 * exercise the pure functions without a board.  Linked with the same
 * cam-ae.c the package builds.
 *
 *   ae MEAN TARGET EXP GAIN  -> cdp_ae_step, prints "rc=N exp=E gain=G"
 *   regs exp V               -> the 0x3e00/0x3e01/0x3e02 bytes, "%02x %02x %02x"
 *   regs gain IDX            -> the 0x3e07/0x3e06/0x3e09 bytes
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "cam-ae.h"

int main(int argc, char **argv)
{
	if (argc >= 3 && !strcmp(argv[1], "regs") &&
	    !strcmp(argv[2], "exp") && argc == 4) {
		unsigned v = (unsigned)strtoul(argv[3], NULL, 0);

		printf("%02x %02x %02x\n",
		       CDP_EXP_H(v), CDP_EXP_M(v), CDP_EXP_L(v));
		return 0;
	}
	if (argc >= 3 && !strcmp(argv[1], "regs") &&
	    !strcmp(argv[2], "gain") && argc == 4) {
		unsigned df, dc, ag;

		cdp_gain_regs((unsigned)strtoul(argv[3], NULL, 0),
			      &df, &dc, &ag);
		printf("%02x %02x %02x\n", df, dc, ag);
		return 0;
	}
	if (argc == 6 && !strcmp(argv[1], "ae")) {
		unsigned exp = (unsigned)strtoul(argv[4], NULL, 0);
		unsigned gain = (unsigned)strtoul(argv[5], NULL, 0);
		int rc = cdp_ae_step((unsigned)strtoul(argv[2], NULL, 0),
				     (unsigned)strtoul(argv[3], NULL, 0),
				     &exp, &gain);

		printf("rc=%d exp=%u gain=%u\n", rc, exp, gain);
		return 0;
	}
	fprintf(stderr, "usage: %s ae M T E G | regs exp V | regs gain I\n",
		argv[0]);
	return 2;
}
