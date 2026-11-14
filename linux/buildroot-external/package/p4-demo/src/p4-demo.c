// SPDX-License-Identifier: GPL-2.0-only
#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <inttypes.h>
#include <limits.h>
#include <poll.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/sysinfo.h>
#include <sys/utsname.h>
#include <time.h>
#include <unistd.h>

#define FRAME_BYTES 256
#define FRAME_MAGIC 0x50344445u
#define IO_TIMEOUT_MS 10000

static int64_t now_ms(void)
{
	struct timespec ts;
	if (clock_gettime(CLOCK_MONOTONIC, &ts))
		return -1;
	return (int64_t)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
}

static int wait_ready(int fd, short events, int64_t deadline)
{
	struct pollfd p = { .fd = fd, .events = events };
	for (;;) {
		int64_t now = now_ms();
		if (now < 0)
			return -1;
		if (now >= deadline) {
			errno = ETIMEDOUT;
			return -1;
		}
		int rc = poll(&p, 1, (int)(deadline - now));
		if (rc < 0 && errno == EINTR)
			continue;
		if (rc < 0)
			return -1;
		/* Read any queued data before interpreting a simultaneous hangup. */
		if (rc > 0 && (p.revents & events))
			return 0;
		if (rc > 0) {
			errno = ECONNRESET;
			return -1;
		}
	}
}

static int nonblocking(int fd)
{
	int flags = fcntl(fd, F_GETFL);
	return flags < 0 ? -1 : fcntl(fd, F_SETFL, flags | O_NONBLOCK);
}

static int transfer(int fd, unsigned char *buf, int sending, int64_t deadline)
{
	size_t done = 0;
	while (done < FRAME_BYTES) {
		if (wait_ready(fd, sending ? POLLOUT : POLLIN, deadline))
			return -1;
		ssize_t n = sending ? send(fd, buf + done, FRAME_BYTES - done, MSG_NOSIGNAL)
				    : recv(fd, buf + done, FRAME_BYTES - done, 0);
		if (n < 0 && (errno == EINTR || errno == EAGAIN || errno == EWOULDBLOCK))
			continue;
		if (n <= 0) {
			if (!n)
				errno = ECONNRESET;
			return -1;
		}
		done += (size_t)n;
	}
	return 0;
}

static void frame(unsigned char *buf, uint32_t sequence)
{
	uint32_t magic = htonl(FRAME_MAGIC), seq = htonl(sequence);
	memcpy(buf, &magic, sizeof(magic));
	memcpy(buf + 4, &seq, sizeof(seq));
	for (size_t i = 8; i < FRAME_BYTES; ++i)
		buf[i] = (unsigned char)(sequence + i);
}

static int serve(unsigned int port, unsigned int count)
{
	int listener = -1, client = -1, result = 1;
	struct sockaddr_in address = {
		.sin_family = AF_INET,
		.sin_port = htons((uint16_t)port),
		.sin_addr.s_addr = htonl(INADDR_ANY),
	};
	listener = socket(AF_INET, SOCK_STREAM, 0);
	if (listener < 0)
		goto fail;
	int reuse = 1;
	if (setsockopt(listener, SOL_SOCKET, SO_REUSEADDR, &reuse, sizeof(reuse)) ||
	    nonblocking(listener) || bind(listener, (struct sockaddr *)&address, sizeof(address)) ||
	    listen(listener, 1))
		goto fail;
	printf("p4-demo listening port=%u messages=%u frame_bytes=%u\n", port, count, FRAME_BYTES);
	fflush(stdout);
	int64_t start = now_ms();
	if (start < 0)
		goto fail;
	for (;;) {
		if (wait_ready(listener, POLLIN, start + 60000))
			goto fail;
		client = accept(listener, NULL, NULL);
		if (client >= 0)
			break;
		if (errno != EINTR && errno != EAGAIN && errno != EWOULDBLOCK)
			goto fail;
	}
	if (nonblocking(client))
		goto fail;
	for (uint32_t sequence = 1; sequence <= count; ++sequence) {
		unsigned char received[FRAME_BYTES], expected[FRAME_BYTES];
		start = now_ms();
		if (start < 0 || transfer(client, received, 0, start + IO_TIMEOUT_MS))
			goto fail;
		frame(expected, sequence);
		if (memcmp(received, expected, sizeof(expected))) {
			fprintf(stderr, "p4-demo payload mismatch sequence=%" PRIu32 "\n", sequence);
			goto out;
		}
		/* One deadline covers both directions; short I/O cannot extend it. */
		if (transfer(client, received, 1, start + IO_TIMEOUT_MS))
			goto fail;
	}
	printf("p4-demo verified_messages=%u payload_bytes=%u\n", count, count * FRAME_BYTES);
	result = 0;
	goto out;
fail:
	perror("p4-demo socket exchange");
out:
	if (client >= 0)
		close(client);
	if (listener >= 0)
		close(listener);
	return result;
}

static int number(const char *s, unsigned int maximum, unsigned int *value)
{
	char *end;
	errno = 0;
	unsigned long n = strtoul(s, &end, 10);
	if (!*s || *end || errno || !n || n > maximum)
		return -1;
	*value = (unsigned int)n;
	return 0;
}

int main(int argc, char **argv)
{
	if (argc == 2 && !strcmp(argv[1], "status")) {
		struct sysinfo info;
		struct utsname name;
		if (sysinfo(&info) || uname(&name)) {
			perror("p4-demo status");
			return 1;
		}
		printf("kernel=%s machine=%s uptime_seconds=%ld\n", name.release, name.machine, info.uptime);
		printf("ram_total_bytes=%" PRIu64 " ram_free_bytes=%" PRIu64 "\n",
		       (uint64_t)info.totalram * info.mem_unit, (uint64_t)info.freeram * info.mem_unit);
		return 0;
	}
	unsigned int port, count = 8;
	if ((argc == 3 || argc == 4) && !strcmp(argv[1], "serve") &&
	    !number(argv[2], 65535, &port) &&
	    (argc == 3 || !number(argv[3], 65535, &count)))
		return serve(port, count);
	fprintf(stderr, "Usage: %s status | serve <port> [messages:1..65535]\n", argv[0]);
	return 2;
}
