// SPDX-License-Identifier: GPL-2.0-only
/*
 * blekbd-att-harness: host-side check of ble-kbd's ATT client framing
 * (#41) for linux/tests/test-ble-kbd-att.sh.  It includes the very
 * ble-kbd.c the package builds (main renamed) and plays the keyboard
 * over a socketpair: a child process is a GATT server with a single
 * attribute and prints every PDU it receives.
 *
 *   read LEN MTU [mtureq]
 *       att_read() a LEN-byte attribute (handle 0x002b) with ATT_MTU
 *       MTU.  With "mtureq" the keyboard sends its own Exchange MTU
 *       Request in the middle of the first transaction.  Prints one
 *       "req xx xx .." line per PDU the keyboard received, then
 *       "read=N ok=0|1".
 *   peer HEX..
 *       hand one PDU "from the keyboard" to att_peer_pdu(); prints
 *       "handled=N rsp=xx xx .." (rsp=none when nothing was sent).
 */
#define main ble_kbd_main
#include "ble-kbd.c"
#undef main

#define TEST_HANDLE	0x002b

static void put_hex(const char *tag, const uint8_t *b, int n)
{
	char line[3 * 600 + 16];
	int o = snprintf(line, sizeof(line), "%s", tag);

	for (int i = 0; i < n && o < (int)sizeof(line) - 4; i++)
		o += snprintf(line + o, sizeof(line) - o, " %02x", b[i]);
	line[o++] = '\n';
	if (write(1, line, o) < 0)
		_exit(3);
}

/* the keyboard: one attribute of `len` bytes, answers Read / Read Blob,
 * rejects anything malformed with Invalid PDU like a real stack does */
static void keyboard(int fd, const uint8_t *val, int len, int mtu,
		     int mtureq)
{
	uint8_t b[600], r[600];

	for (;;) {
		ssize_t n = read(fd, b, sizeof(b));
		int off;

		if (n <= 0)
			_exit(0);
		put_hex("req", b, (int)n);
		if (b[0] == ATT_OP_MTU_RSP)
			continue;	/* the answer to our own request */
		if (b[0] == ATT_OP_READ_REQ && n == 3) {
			off = 0;
		} else if (b[0] == ATT_OP_READ_BLOB_REQ && n == 5) {
			off = b[3] | (b[4] << 8);
		} else {
			uint8_t e[5] = { ATT_OP_ERROR, b[0],
					 n > 1 ? b[1] : 0, n > 2 ? b[2] : 0,
					 0x04 /* Invalid PDU */ };

			if (write(fd, e, sizeof(e)) < 0)
				_exit(3);
			continue;
		}
		if ((b[1] | (b[2] << 8)) != TEST_HANDLE || off > len) {
			uint8_t e[5] = { ATT_OP_ERROR, b[0], b[1], b[2],
					 off > len ? 0x07 : 0x01 };

			if (write(fd, e, sizeof(e)) < 0)
				_exit(3);
			continue;
		}
		if (mtureq) {
			/* the keyboard is a GATT client too */
			uint8_t m[3] = { ATT_OP_MTU_REQ, 23, 0 };

			mtureq = 0;
			if (write(fd, m, sizeof(m)) < 0)
				_exit(3);
		}
		int chunk = len - off;

		if (chunk > mtu - 1)
			chunk = mtu - 1;
		r[0] = b[0] + 1;
		memcpy(r + 1, val + off, chunk);
		if (write(fd, r, 1 + chunk) < 0)
			_exit(3);
	}
}

static int hexbyte(const char *s)
{
	return (int)strtoul(s, NULL, 16) & 0xff;
}

int main(int argc, char **argv)
{
	int sv[2];

	if (socketpair(AF_UNIX, SOCK_SEQPACKET, 0, sv) < 0) {
		perror("socketpair");
		return 2;
	}

	if (argc >= 4 && !strcmp(argv[1], "read")) {
		static uint8_t val[1024], out[1024];
		int len = atoi(argv[2]);
		int mtu = atoi(argv[3]);
		int mtureq = argc > 4 && !strcmp(argv[4], "mtureq");

		if (len < 0 || len > (int)sizeof(val) || mtu < 23 || mtu > 517)
			return 2;
		for (int i = 0; i < len; i++)
			val[i] = (uint8_t)(i * 7 + 3);

		pid_t pid = fork();

		if (pid < 0)
			return 2;
		if (pid == 0) {
			close(sv[0]);
			keyboard(sv[1], val, len, mtu, mtureq);
			_exit(0);
		}
		close(sv[1]);
		att_fd = sv[0];
		att_mtu = (uint16_t)mtu;

		int n = att_read(TEST_HANDLE, out, sizeof(out));

		close(att_fd);
		waitpid(pid, NULL, 0);
		printf("read=%d ok=%d\n", n,
		       n == len && !memcmp(out, val, len));
		return 0;
	}

	if (argc >= 3 && !strcmp(argv[1], "peer")) {
		uint8_t pdu[64], rsp[64];
		int n = 0;
		struct pollfd pfd = { .fd = sv[1], .events = POLLIN };

		for (int i = 2; i < argc && n < (int)sizeof(pdu); i++)
			pdu[n++] = (uint8_t)hexbyte(argv[i]);
		att_fd = sv[0];

		int handled = att_peer_pdu(pdu, n);
		char tag[32];

		snprintf(tag, sizeof(tag), "handled=%d rsp=", handled);
		if (poll(&pfd, 1, 100) > 0) {
			ssize_t r = read(sv[1], rsp, sizeof(rsp));

			put_hex(tag, rsp, r > 0 ? (int)r : 0);
		} else {
			printf("%snone\n", tag);
		}
		return 0;
	}

	fprintf(stderr, "usage: %s read LEN MTU [mtureq] | peer HEX..\n",
		argv[0]);
	return 2;
}
