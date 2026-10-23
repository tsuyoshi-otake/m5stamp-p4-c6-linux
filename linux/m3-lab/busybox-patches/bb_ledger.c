/* vi: set sw=4 ts=4: */
//kbuild:lib-y += easystick_bb_ledger.o

/* EasyStick M2.5 post-exec BusyBox ledger implementation. */
#include "easystick_bb_ledger.h"

#include <errno.h>
#include <fcntl.h>
#include <string.h>
#include <unistd.h>

static int es_bb_kmsg_fd = -1;
static int es_bb_tty_fd = -1;

static void es_bb_write_sinks(const char *msg, size_t len)
{
	if (es_bb_kmsg_fd >= 0)
		(void)write(es_bb_kmsg_fd, msg, len);
	if (es_bb_tty_fd >= 0)
		(void)write(es_bb_tty_fd, msg, len);
}

void es_bb_ledger_open(void)
{
	int saved = errno;

	if (es_bb_kmsg_fd < 0)
		es_bb_kmsg_fd = open("/dev/kmsg", O_WRONLY | O_CLOEXEC | O_NONBLOCK);
	if (es_bb_tty_fd < 0)
		es_bb_tty_fd = open("/dev/ttyGS1", O_WRONLY | O_CLOEXEC | O_NONBLOCK);
	errno = saved;
}

void es_bb_mark(const char *msg)
{
	int saved = errno;
	size_t len;

	if (!msg) {
		errno = saved;
		return;
	}
	if (es_bb_kmsg_fd < 0 || es_bb_tty_fd < 0)
		es_bb_ledger_open();
	len = strlen(msg);
	es_bb_write_sinks(msg, len);
	errno = saved;
}
