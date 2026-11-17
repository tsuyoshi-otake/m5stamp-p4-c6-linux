// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * hci-adv -- start LE advertising over the raw HCI socket ABI (#34).
 *
 * Same NOMMU rationale as hci-scan: a private copy of the HCI socket
 * ABI, HCI_CHANNEL_USER exclusive bind, then LE Set Advertising
 * Parameters + Data + Enable.  Advertises ADV_IND with flags and a
 * Complete Local Name (default: hostname).
 *
 * Usage: hci-adv [seconds] [name]
 *        seconds <= 0 or omitted: advertise until killed.
 */
#include <errno.h>
#include <poll.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/socket.h>
#include <time.h>
#include <unistd.h>

#ifndef AF_BLUETOOTH
#define AF_BLUETOOTH	31
#endif
#define BTPROTO_HCI	1
#define SOL_HCI		0
#define HCI_FILTER	2
#define HCIDEVDOWN	_IOW('H', 202, int)

struct sockaddr_hci {
	sa_family_t	hci_family;
	uint16_t	hci_dev;
	uint16_t	hci_channel;
};
#define HCI_CHANNEL_RAW		0
#define HCI_CHANNEL_USER	1

struct hci_filter {
	uint32_t	type_mask;
	uint32_t	event_mask[2];
	uint16_t	opcode;
};

#define HCI_COMMAND_PKT	0x01
#define HCI_EVENT_PKT	0x04

#define EVT_CMD_COMPLETE	0x0e
#define EVT_CMD_STATUS		0x0f
#define EVT_LE_META_EVENT	0x3e

#define OGF_HOST_CTL			0x03
#define OCF_RESET			0x0003
#define OGF_LE_CTL			0x08
#define OCF_LE_SET_ADVERTISING_PARAMETERS	0x0006
#define OCF_LE_SET_ADVERTISING_DATA		0x0008
#define OCF_LE_SET_ADVERTISE_ENABLE		0x000a
#define OGF_INFO_PARAM		0x04
#define OCF_READ_BD_ADDR	0x0009

static volatile sig_atomic_t quit;

static void on_sig(int sig) { (void)sig; quit = 1; }

static int hci_open(int dev)
{
	struct sockaddr_hci sa;
	struct hci_filter flt;
	int fd;

	fd = socket(AF_BLUETOOTH, SOCK_RAW | SOCK_CLOEXEC, BTPROTO_HCI);
	if (fd < 0) {
		perror("socket");
		return -1;
	}
	memset(&sa, 0, sizeof(sa));
	sa.hci_family = AF_BLUETOOTH;
	sa.hci_dev = dev;
	sa.hci_channel = HCI_CHANNEL_USER;
	if (bind(fd, (struct sockaddr *)&sa, sizeof(sa)) < 0) {
		if (errno == EBUSY) {
			int ctl = socket(AF_BLUETOOTH, SOCK_RAW, BTPROTO_HCI);

			if (ctl >= 0) {
				struct sockaddr_hci ca = {
					.hci_family = AF_BLUETOOTH,
					.hci_dev = dev,
					.hci_channel = HCI_CHANNEL_RAW,
				};

				if (bind(ctl, (struct sockaddr *)&ca,
					 sizeof(ca)) == 0)
					ioctl(ctl, HCIDEVDOWN, dev);
				close(ctl);
			}
			if (bind(fd, (struct sockaddr *)&sa, sizeof(sa)) == 0)
				goto bound;
		}
		perror("bind hci-user");
		close(fd);
		return -1;
	}
bound:
	memset(&flt, 0, sizeof(flt));
	flt.type_mask = 1u << HCI_EVENT_PKT;
	flt.event_mask[0] = (1u << EVT_CMD_COMPLETE) | (1u << EVT_CMD_STATUS);
	flt.event_mask[1] = 0xffffffff;	/* keep LE meta etc. out of the way */
	setsockopt(fd, SOL_HCI, HCI_FILTER, &flt, sizeof(flt));
	return fd;
}

static int hci_cmd(int fd, uint16_t ogf, uint16_t ocf,
		   const void *params, uint8_t plen)
{
	uint8_t pkt[260];
	uint16_t op = (ogf << 10) | ocf;

	pkt[0] = HCI_COMMAND_PKT;
	pkt[1] = op & 0xff;
	pkt[2] = op >> 8;
	pkt[3] = plen;
	if (plen)
		memcpy(pkt + 4, params, plen);
	return write(fd, pkt, 4 + plen);
}

static uint16_t hci_opcode(uint16_t ogf, uint16_t ocf)
{
	return (ogf << 10) | ocf;
}

static int hci_wait_cmpl(int fd, uint16_t opcode, uint8_t *rparam, int rlen)
{
	uint8_t buf[300];
	struct pollfd pfd = { .fd = fd, .events = POLLIN };
	int i;

	for (i = 0; i < 60; i++) {
		int n;

		if (poll(&pfd, 1, 200) <= 0)
			continue;
		n = read(fd, buf, sizeof(buf));
		if (n < 4 || buf[0] != HCI_EVENT_PKT)
			continue;
		if (buf[1] == EVT_CMD_COMPLETE && n >= 6) {
			uint16_t op = buf[4] | (buf[5] << 8);

			if (op == opcode) {
				if (rparam && rlen && n > 6)
					memcpy(rparam, buf + 6,
					       n - 6 < rlen ? n - 6 : rlen);
				return n > 6 ? buf[6] : 0;
			}
		} else if (buf[1] == EVT_CMD_STATUS && n >= 7) {
			uint16_t op = buf[5] | (buf[6] << 8);

			if (op == opcode && buf[3] != 0)
				return buf[3];
		}
	}
	return -1;
}

int main(int argc, char **argv)
{
	int secs = argc > 1 ? atoi(argv[1]) : 0;
	const char *name = argc > 2 ? argv[2] : NULL;
	char host[32];
	uint8_t prm[40], bd[8], *d;
	struct timespec end, now;
	int fd, st = -1, namelen;

	if (!name) {
		host[0] = 0;
		gethostname(host, sizeof(host) - 1);
		name = host[0] ? host : "tab5";
	}
	namelen = strlen(name);
	if (namelen > 28)
		namelen = 28;

	fd = hci_open(0);
	if (fd < 0)
		return 1;

	/* The hosted controller rejects LE commands with 0x0c
	 * (command disallowed) until a Reset puts it in the idle
	 * state. */
	if (hci_cmd(fd, OGF_HOST_CTL, OCF_RESET, NULL, 0) < 0 ||
	    (st = hci_wait_cmpl(fd, hci_opcode(OGF_HOST_CTL, OCF_RESET),
				NULL, 0)) != 0) {
		fprintf(stderr, "reset failed (status 0x%02x)\n", st);
		return 1;
	}

	if (hci_cmd(fd, OGF_INFO_PARAM, OCF_READ_BD_ADDR, NULL, 0) > 0 &&
	    hci_wait_cmpl(fd, hci_opcode(OGF_INFO_PARAM, OCF_READ_BD_ADDR),
			  bd, sizeof(bd)) == 0)
		printf("local %02x:%02x:%02x:%02x:%02x:%02x\n",
		       bd[6], bd[5], bd[4], bd[3], bd[2], bd[1]);

	/* adv params: 100 ms interval, ADV_IND, all channels */
	memset(prm, 0, sizeof(prm));
	prm[0] = 0xa0; prm[1] = 0x00;		/* min interval 100 ms */
	prm[2] = 0xa0; prm[3] = 0x00;		/* max interval 100 ms */
	prm[4] = 0x00;				/* ADV_IND */
	prm[5] = 0x00;				/* own addr public */
	prm[6] = 0x00;				/* peer addr type */
	/* prm[7..12] peer addr = 0 */
	prm[13] = 0x07;				/* channels 37+38+39 */
	prm[14] = 0x00;				/* no filter */
	if (hci_cmd(fd, OGF_LE_CTL, OCF_LE_SET_ADVERTISING_PARAMETERS,
		    prm, 15) < 0 ||
	    (st = hci_wait_cmpl(fd, hci_opcode(OGF_LE_CTL,
				OCF_LE_SET_ADVERTISING_PARAMETERS),
				NULL, 0)) != 0) {
		fprintf(stderr, "adv params failed (status 0x%02x)\n", st);
		return 1;
	}

	/* adv data: flags(0x06) + complete local name */
	memset(prm, 0, sizeof(prm));
	d = prm + 1;
	*d++ = 2; *d++ = 0x01; *d++ = 0x06;		/* flags */
	*d++ = 1 + namelen; *d++ = 0x09;
	memcpy(d, name, namelen);
	d += namelen;
	prm[0] = d - prm - 1;
	if (hci_cmd(fd, OGF_LE_CTL, OCF_LE_SET_ADVERTISING_DATA,
		    prm, 32) < 0 ||
	    (st = hci_wait_cmpl(fd, hci_opcode(OGF_LE_CTL,
				OCF_LE_SET_ADVERTISING_DATA),
				NULL, 0)) != 0) {
		fprintf(stderr, "adv data failed (status 0x%02x)\n", st);
		return 1;
	}

	prm[0] = 0x01;
	if (hci_cmd(fd, OGF_LE_CTL, OCF_LE_SET_ADVERTISE_ENABLE, prm, 1) < 0 ||
	    (st = hci_wait_cmpl(fd, hci_opcode(OGF_LE_CTL,
				OCF_LE_SET_ADVERTISE_ENABLE),
				NULL, 0)) != 0) {
		fprintf(stderr, "adv enable failed (status 0x%02x)\n", st);
		return 1;
	}
	printf("advertising as \"%s\"%s\n", name,
	       secs > 0 ? "" : " (until killed)");
	fflush(stdout);

	signal(SIGINT, on_sig);
	signal(SIGTERM, on_sig);
	if (secs > 0) {
		clock_gettime(CLOCK_MONOTONIC, &end);
		end.tv_sec += secs;
		do {
			struct timespec req = { .tv_sec = 0,
						.tv_nsec = 200000000 };
			nanosleep(&req, NULL);
			clock_gettime(CLOCK_MONOTONIC, &now);
		} while (!quit &&
			 (now.tv_sec < end.tv_sec ||
			  (now.tv_sec == end.tv_sec &&
			   now.tv_nsec < end.tv_nsec)));
	} else {
		while (!quit)
			pause();
	}

	prm[0] = 0x00;
	hci_cmd(fd, OGF_LE_CTL, OCF_LE_SET_ADVERTISE_ENABLE, prm, 1);
	hci_wait_cmpl(fd, hci_opcode(OGF_LE_CTL, OCF_LE_SET_ADVERTISE_ENABLE),
		      NULL, 0);
	close(fd);
	return 0;
}
