// SPDX-License-Identifier: GPL-2.0-only
/*
 * Restart a daemon whenever it exits (#29).  The init scripts used
 * "( while :; do daemon; sleep 2; done ) &", but on NOMMU that subshell is a
 * fresh copy of BusyBox with its own ~500 KB of text and data, and three of
 * them left too little contiguous memory for fork+exec (first-boot OOM).
 *
 * usage: easystick-respawn [-d SECS] [-p PIDFILE] -- PROG [ARG...]
 *
 * -d  delay before each restart (default 2)
 * -p  file that holds the running child's PID; removed while none runs
 *
 * SIGTERM/SIGINT/SIGHUP stop the child with SIGTERM and then exit, so killing
 * the supervisor also stops the daemon.
 */
#include <errno.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/wait.h>
#include <unistd.h>

static volatile sig_atomic_t stopping;
static volatile pid_t child;
static const char *pidfile;
/* Static: locals live across vfork() and may be clobbered. */
static unsigned int delay = 2;

static void on_stop(int sig)
{
	(void)sig;
	stopping = 1;
	if (child > 0)
		kill(child, SIGTERM);
}

static void write_pid(pid_t pid)
{
	FILE *f;

	if (!pidfile)
		return;
	f = fopen(pidfile, "w");
	if (!f)
		return;
	fprintf(f, "%d\n", (int)pid);
	fclose(f);
}

int main(int argc, char **argv)
{
	struct sigaction sa;
	int opt, status;
	pid_t pid;

	while ((opt = getopt(argc, argv, "+d:p:")) != -1) {
		switch (opt) {
		case 'd':
			delay = (unsigned int)strtoul(optarg, NULL, 10);
			break;
		case 'p':
			pidfile = optarg;
			break;
		default:
			goto usage;
		}
	}
	if (optind >= argc)
		goto usage;

	memset(&sa, 0, sizeof(sa));
	sa.sa_handler = on_stop;
	sigaction(SIGTERM, &sa, NULL);
	sigaction(SIGINT, &sa, NULL);
	sigaction(SIGHUP, &sa, NULL);

	while (!stopping) {
		/* NOMMU has no fork(); vfork + exec is the only way to spawn. */
		pid = vfork();
		if (pid == 0) {
			execvp(argv[optind], &argv[optind]);
			_exit(127);
		}
		if (pid < 0) {
			perror("easystick-respawn: vfork");
		} else {
			child = pid;
			write_pid(pid);
			/* A signal that raced the vfork() did not see the child. */
			if (stopping)
				kill(pid, SIGTERM);
			while (waitpid(pid, &status, 0) < 0 && errno == EINTR)
				;
			child = 0;
			if (pidfile)
				unlink(pidfile);
		}
		if (!stopping)
			sleep(delay);
	}
	return 0;

usage:
	fprintf(stderr, "usage: %s [-d SECS] [-p PIDFILE] -- PROG [ARG...]\n",
		argv[0]);
	return 2;
}
