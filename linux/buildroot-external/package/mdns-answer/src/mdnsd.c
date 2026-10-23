// SPDX-License-Identifier: GPL-2.0-only
/*
 * Minimal mDNS responder (#25): answers A queries for <hostname>.local with
 * the current IPv4 address of one interface.  No service discovery, no
 * probing, no cache.  mdnsd/avahi need fork() and do not build for NOMMU.
 *
 * usage: mdnsd [-i IFNAME] [-p PORT]
 */
#include <arpa/inet.h>
#include <errno.h>
#include <net/if.h>
#include <netinet/in.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/ioctl.h>
#include <sys/socket.h>
#include <unistd.h>

#define MDNS_ADDR "224.0.0.251"
#define TYPE_A 1
#define TYPE_ANY 255
#define CLASS_IN 1
#define TTL_S 120

/* Static buffers keep the 4 KB bFLT stack free. */
static uint8_t rx[1500];
static uint8_t tx[512];

static const char *ifname = "wlan0";
static int port = 5353;

static int if_ipv4(int fd, struct in_addr *out)
{
	struct ifreq ifr;

	memset(&ifr, 0, sizeof(ifr));
	strncpy(ifr.ifr_name, ifname, IFNAMSIZ - 1);
	if (ioctl(fd, SIOCGIFADDR, &ifr) < 0)
		return -1;
	*out = ((struct sockaddr_in *)&ifr.ifr_addr)->sin_addr;
	return out->s_addr ? 0 : -1;
}

static int join_group(int fd)
{
	struct ip_mreqn mreq;

	memset(&mreq, 0, sizeof(mreq));
	inet_pton(AF_INET, MDNS_ADDR, &mreq.imr_multiaddr);
	mreq.imr_ifindex = (int)if_nametoindex(ifname);
	if (!mreq.imr_ifindex)
		return -1;
	if (setsockopt(fd, IPPROTO_IP, IP_ADD_MEMBERSHIP, &mreq, sizeof(mreq)) < 0 &&
	    errno != EADDRINUSE)
		return -1;
	setsockopt(fd, IPPROTO_IP, IP_MULTICAST_IF, &mreq, sizeof(mreq));
	return 0;
}

/*
 * Read the question name at rx[*off] into dotted form.  Questions are not
 * compressed in practice; a pointer makes us skip the packet.
 */
static int read_name(size_t len, size_t *off, char *name, size_t cap)
{
	size_t o = *off, n = 0;

	for (;;) {
		uint8_t l;

		if (o >= len)
			return -1;
		l = rx[o++];
		if (!l)
			break;
		if (l & 0xc0 || o + l > len || n + l + 2 > cap)
			return -1;
		if (n)
			name[n++] = '.';
		memcpy(name + n, rx + o, l);
		n += l;
		o += l;
	}
	name[n] = '\0';
	*off = o;
	return 0;
}

static size_t put_name(uint8_t *p, const char *host)
{
	size_t n = 0, l = strlen(host);

	p[n++] = (uint8_t)l;
	memcpy(p + n, host, l);
	n += l;
	p[n++] = 5;
	memcpy(p + n, "local", 5);
	n += 5;
	p[n++] = 0;
	return n;
}

static void put16(uint8_t *p, unsigned int v)
{
	p[0] = (uint8_t)(v >> 8);
	p[1] = (uint8_t)v;
}

int main(int argc, char **argv)
{
	struct sockaddr_in sa;
	int fd, opt, one = 1, joined = 0;

	while ((opt = getopt(argc, argv, "i:p:")) != -1) {
		if (opt == 'i')
			ifname = optarg;
		else if (opt == 'p')
			port = atoi(optarg);
		else
			return 2;
	}

	fd = socket(AF_INET, SOCK_DGRAM, 0);
	if (fd < 0) {
		perror("mdnsd: socket");
		return 1;
	}
	setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));
	memset(&sa, 0, sizeof(sa));
	sa.sin_family = AF_INET;
	sa.sin_port = htons((uint16_t)port);
	if (bind(fd, (struct sockaddr *)&sa, sizeof(sa)) < 0) {
		perror("mdnsd: bind");
		return 1;
	}
	{
		int ttl = 255;

		setsockopt(fd, IPPROTO_IP, IP_MULTICAST_TTL, &ttl, sizeof(ttl));
	}

	for (;;) {
		struct sockaddr_in from, to;
		socklen_t fl = sizeof(from);
		struct in_addr ip;
		char host[64], q[256], want[80];
		size_t off, len, n;
		unsigned int qd, i, id;
		ssize_t r;
		int match = 0, unicast, compat = 0;
		const char *answer = host;

		/* wlan0 may come and go with S40network reloads. */
		if (!joined) {
			if (join_group(fd) == 0) {
				joined = 1;
			} else {
				sleep(5);
				continue;
			}
		}
		r = recvfrom(fd, rx, sizeof(rx), 0, (struct sockaddr *)&from, &fl);
		if (r < 12)
			continue;
		len = (size_t)r;
		/* Only standard queries. */
		if (rx[2] & 0xf8)
			continue;
		id = (unsigned int)rx[0] << 8 | rx[1];
		qd = (unsigned int)rx[4] << 8 | rx[5];
		if (gethostname(host, sizeof(host) - 1) < 0)
			continue;
		host[sizeof(host) - 1] = '\0';
		/* A DNS label is at most 63 bytes. */
		if (!host[0] || strlen(host) > 63)
			continue;
		snprintf(want, sizeof(want), "%s.local", host);

		off = 12;
		for (i = 0; i < qd; i++) {
			unsigned int type, cls;

			if (read_name(len, &off, q, sizeof(q)) < 0 || off + 4 > len)
				break;
			type = (unsigned int)rx[off] << 8 | rx[off + 1];
			cls = ((unsigned int)rx[off + 2] << 8 | rx[off + 3]) & 0x7fff;
			off += 4;
			if (cls == CLASS_IN && (type == TYPE_A || type == TYPE_ANY) &&
			    !strcasecmp(q, want)) {
				match = 1;
				break;
			}
			/* #55 compat, remove after v0.5.x: answer the old name. */
			if (cls == CLASS_IN && (type == TYPE_A || type == TYPE_ANY) &&
			    !strcasecmp(q, "easystick.local")) {
				match = 1;
				compat = 1;
				break;
			}
			/* #55 compat end */
		}
		if (!match)
			continue;
		if (if_ipv4(fd, &ip) < 0) {
			/* Interface lost its address: rejoin once it is back. */
			joined = 0;
			continue;
		}

		/* A query from a port other than the mDNS port is a legacy unicast query:
		 * answer to the sender, echo the ID and the question. */
		unicast = ntohs(from.sin_port) != port;
		/* #55 compat, remove after v0.5.x */
		if (compat)
			answer = "easystick";
		/* #55 compat end */
		memset(tx, 0, 12);
		n = 12;
		put16(tx + 2, 0x8400);
		put16(tx + 6, 1);
		if (unicast) {
			put16(tx, id);
			put16(tx + 4, 1);
			n += put_name(tx + n, answer);
			put16(tx + n, TYPE_A);
			put16(tx + n + 2, CLASS_IN);
			n += 4;
		}
		n += put_name(tx + n, answer);
		put16(tx + n, TYPE_A);
		put16(tx + n + 2, unicast ? CLASS_IN : 0x8000 | CLASS_IN);
		tx[n + 4] = 0;
		tx[n + 5] = 0;
		put16(tx + n + 6, TTL_S);
		put16(tx + n + 8, 4);
		memcpy(tx + n + 10, &ip, 4);
		n += 14;

		if (unicast) {
			to = from;
		} else {
			memset(&to, 0, sizeof(to));
			to.sin_family = AF_INET;
			to.sin_port = htons((uint16_t)port);
			inet_pton(AF_INET, MDNS_ADDR, &to.sin_addr);
		}
		sendto(fd, tx, n, 0, (struct sockaddr *)&to, sizeof(to));
	}
}
