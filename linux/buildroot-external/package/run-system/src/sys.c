// SPDX-License-Identifier: GPL-2.0-only
/*
 * Mark this task "system" for the #51 NOMMU user-pool reserve gate, then
 * exec the target.  The mark is PF_SYSTEM_WORK in task flags: it
 * survives exec and is inherited by fork, so a script that re-execs
 * itself through run-system marks its whole subtree.  Only tasks
 * with CAP_SYS_ADMIN may set it (enforced kernel-side); on kernels
 * without the option the prctl fails with EINVAL and the target still
 * runs unmarked.
 *
 * usage: run-system PROG [ARG...]      mark and exec
 *        run-system -u PROG [ARG...]   clear the mark, then exec
 */
#include <errno.h>
#include <stdio.h>
#include <string.h>
#include <sys/prctl.h>
#include <unistd.h>

#ifndef PR_SET_SYSTEM_WORK
#define PR_SET_SYSTEM_WORK 0x45535459 /* 'ESTY' */
#endif

int main(int argc, char **argv)
{
	long mark = 1;

	if (argc >= 2 && !strcmp(argv[1], "-u")) {
		mark = 0;
		argv++;
		argc--;
	}
	if (argc < 2) {
		fprintf(stderr, "usage: %s [-u] PROG [ARG...]\n", argv[0]);
		return 2;
	}
	/* EINVAL (old kernel), EPERM (not root): run the target anyway. */
	if (prctl(PR_SET_SYSTEM_WORK, mark, 0, 0, 0) && errno != EINVAL &&
	    errno != EPERM)
		fprintf(stderr, "run-system: prctl: %s\n", strerror(errno));
	execvp(argv[1], argv + 1);
	fprintf(stderr, "run-system: exec %s: %s\n", argv[1],
		strerror(errno));
	/* 111: callers distinguish "could not exec" from a real exit code
	 * and fall back to running the target unmarked. */
	return 111;
}
