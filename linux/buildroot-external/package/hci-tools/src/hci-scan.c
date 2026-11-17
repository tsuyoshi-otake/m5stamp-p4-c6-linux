// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * hci-scan -- LE passive scan over the raw HCI socket ABI (#34).
 *
 * BlueZ userland cannot build for this NOMMU target, so the tool
 * carries the minimal HCI socket ABI itself: open an
 * AF_BLUETOOTH/SOCK_RAW/BTPROTO_HCI socket on HCI_CHANNEL_USER
 * (exclusive, dev must be down -- nothing else manages hci0 here),
 * send LE Set Scan Parameters + LE Set Scan Enable, then print each
 * LE Advertising Report for the requested time window.
 *
 * Usage: hci-scan [seconds]   (default 10)
 */
#include <errno.h>
#include <poll.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/socket.h>
#include <time.h>
#include <unistd.h>

/* ---- minimal HCI socket ABI (normally <bluetooth/hci.h>) ---- */
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
#define EVT_LE_ADVERTISING_REPORT	0x02
#define EVT_LE_EXT_ADVERTISING_REPORT	0x0d

#define OGF_HOST_CTL		0x03
#define OCF_RESET		0x0003
#define OCF_SET_EVENT_MASK	0x0001
#define OGF_LE_CTL		0x08
#define OCF_LE_SET_EVENT_MASK	0x0001
#define OCF_LE_SET_SCAN_PARAMETERS	0x000b
#define OCF_LE_SET_SCAN_ENABLE		0x000c
#define OGF_INFO_PARAM		0x04
#define OCF_READ_BD_ADDR		0x0009

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
			/* the dev is up; drop it and take exclusive control */
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
	flt.event_mask[1] = 1u << (EVT_LE_META_EVENT - 32);
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

/* Wait for the command-complete of opcode; returns its status byte,
 * or -1 on timeout.  Consumes other events on the way. */
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

			/* nonzero status means the command died here */
			if (op == opcode && buf[3] != 0)
				return buf[3];
		}
	}
	return -1;
}

int main(int argc, char **argv)
{
	int secs = argc > 1 ? atoi(argv[1]) : 10;
	uint8_t prm[16], bd[8];
	struct timespec end, now;
	int fd, st = -1, ev_total, ev_meta;

	if (secs <= 0)
		secs = 10;

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

	/* enable every event + every LE subevent */
	memset(prm, 0xff, 8);
	if (hci_cmd(fd, OGF_HOST_CTL, OCF_SET_EVENT_MASK, prm, 8) < 0 ||
	    (st = hci_wait_cmpl(fd,
				hci_opcode(OGF_HOST_CTL, OCF_SET_EVENT_MASK),
				NULL, 0)) != 0 ||
	    hci_cmd(fd, OGF_LE_CTL, OCF_LE_SET_EVENT_MASK, prm, 8) < 0 ||
	    (st = hci_wait_cmpl(fd,
				hci_opcode(OGF_LE_CTL, OCF_LE_SET_EVENT_MASK),
				NULL, 0)) != 0) {
		fprintf(stderr, "event masks failed (status 0x%02x)\n", st);
		return 1;
	}

	if (hci_cmd(fd, OGF_INFO_PARAM, OCF_READ_BD_ADDR, NULL, 0) > 0 &&
	    hci_wait_cmpl(fd, hci_opcode(OGF_INFO_PARAM, OCF_READ_BD_ADDR),
			  bd, sizeof(bd)) == 0)
		printf("local %02x:%02x:%02x:%02x:%02x:%02x\n",
		       bd[6], bd[5], bd[4], bd[3], bd[2], bd[1]);

	/* passive scan, ~11 ms interval/window, public own addr */
	prm[0] = 0x00;				/* passive */
	prm[1] = 0x12; prm[2] = 0x00;		/* interval */
	prm[3] = 0x12; prm[4] = 0x00;		/* window */
	prm[5] = 0x00;				/* own addr type */
	prm[6] = 0x00;				/* accept all */
	if (hci_cmd(fd, OGF_LE_CTL, OCF_LE_SET_SCAN_PARAMETERS, prm, 7) < 0 ||
	    (st = hci_wait_cmpl(fd,
				hci_opcode(OGF_LE_CTL,
					   OCF_LE_SET_SCAN_PARAMETERS),
				NULL, 0)) != 0) {
		fprintf(stderr, "scan params failed (status 0x%02x)\n", st);
		return 1;
	}
	prm[0] = 0x01; prm[1] = 0x00;		/* enable, dup filter off */
	if (hci_cmd(fd, OGF_LE_CTL, OCF_LE_SET_SCAN_ENABLE, prm, 2) < 0 ||
	    (st = hci_wait_cmpl(fd,
				hci_opcode(OGF_LE_CTL, OCF_LE_SET_SCAN_ENABLE),
				NULL, 0)) != 0) {
		fprintf(stderr, "scan enable failed (status 0x%02x)\n", st);
		return 1;
	}

	clock_gettime(CLOCK_MONOTONIC, &end);
	end.tv_sec += secs;
	ev_total = ev_meta = 0;
	for (;;) {
		struct pollfd pfd = { .fd = fd, .events = POLLIN };
		uint8_t buf[300];
		int n, i, nr;
		uint8_t *p, *e;
		long ms;

		clock_gettime(CLOCK_MONOTONIC, &now);
		ms = (end.tv_sec - now.tv_sec) * 1000 +
		     (end.tv_nsec - now.tv_nsec) / 1000000;
		if (ms <= 0)
			break;
		if (poll(&pfd, 1, ms < 1000 ? (int)ms : 1000) <= 0)
			continue;
		n = read(fd, buf, sizeof(buf));
		if (n > 0 && buf[0] == HCI_EVENT_PKT)
			ev_total++;
		if (n < 5 || buf[0] != HCI_EVENT_PKT ||
		    buf[1] != EVT_LE_META_EVENT)
			continue;
		ev_meta++;
		if (buf[3] == EVT_LE_EXT_ADVERTISING_REPORT) {
			/* evt_type(2) addr_type(1) addr(6) prim_phy(1)
			 * sec_phy(1) sid(1) tx_pwr(1) rssi(1) period(2)
			 * dir_type(1) dir_addr(6) dlen(1) data(dlen) */
			nr = buf[4];
			p = buf + 5;
			e = buf + 3 + buf[2];
			for (i = 0; i < nr && p + 24 <= e; i++) {
				uint8_t *addr = p + 3;
				uint8_t dlen = p[23], *ad, *adend;
				char name[32] = "";
				int8_t rssi = (int8_t)p[13];

				ad = p + 24;
				adend = ad + dlen;
				if (adend > e)
					break;
				while (ad + 2 <= adend && ad + 1 + ad[0] <= adend) {
					if ((ad[1] == 0x09 || ad[1] == 0x08) &&
					    ad[0] > 1) {
						int l = ad[0] - 1;

						if (l > 30)
							l = 30;
						memcpy(name, ad + 2, l);
						name[l] = 0;
						break;
					}
					ad += 1 + ad[0];
				}
				p += 24 + dlen;
				printf("%02x:%02x:%02x:%02x:%02x:%02x rssi=%d%s %s\n",
				       addr[5], addr[4], addr[3],
				       addr[2], addr[1], addr[0],
				       rssi, name[0] ? " name=" : "", name);
				fflush(stdout);
			}
			continue;
		}
		if (buf[3] != EVT_LE_ADVERTISING_REPORT)
			continue;

		nr = buf[4];
		p = buf + 5;
		e = buf + 3 + buf[2];
		for (i = 0; i < nr && p + 9 <= e; i++) {
			uint8_t *addr = p + 2;
			uint8_t dlen, *ad, *adend;
			char name[32] = "";
			int8_t rssi = 0;

			p += 8;	/* ev_type, addr_type, addr[6] */
			if (p >= e)
				break;
			dlen = *p++;
			if (p + dlen > e)
				break;
			ad = p;
			adend = p + dlen;
			while (ad + 2 <= adend && ad + 1 + ad[0] <= adend) {
				if ((ad[1] == 0x09 || ad[1] == 0x08) && ad[0] > 1) {
					int l = ad[0] - 1;

					if (l > 30)
						l = 30;
					memcpy(name, ad + 2, l);
					name[l] = 0;
					break;
				}
				ad += 1 + ad[0];
			}
			p += dlen;
			if (p < e)
				rssi = (int8_t)*p++;
			printf("%02x:%02x:%02x:%02x:%02x:%02x rssi=%d%s %s\n",
			       addr[5], addr[4], addr[3],
			       addr[2], addr[1], addr[0],
			       rssi, name[0] ? " name=" : "", name);
			fflush(stdout);
		}
	}

	fprintf(stderr, "events: %d total, %d LE-meta\n", ev_total, ev_meta);
	prm[0] = 0x00; prm[1] = 0x00;
	hci_cmd(fd, OGF_LE_CTL, OCF_LE_SET_SCAN_ENABLE, prm, 2);
	hci_wait_cmpl(fd, hci_opcode(OGF_LE_CTL, OCF_LE_SET_SCAN_ENABLE),
		      NULL, 0);
	close(fd);
	return 0;
}
