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
 *   secok LEVEL KEYSIZE WANT
 *       would att_connect() use a link in that BT_SECURITY state as it
 *       is, without encrypting it again?  Prints "enough=0|1".
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

	if (argc >= 3 && !strcmp(argv[1], "holds")) {
		/* holds SEQ: drive the daemon's per-bond hold policy (#41).
		 * r = key refused, f = plain failure, s = served, w = let
		 * the clock run past the current hold.  One line per
		 * session: the note return plus the resulting state. */
		static struct kbd_hold h;
		time_t now = 100000;

		memset(&h, 0, sizeof(h));
		h.used = 1;
		for (const char *p = argv[2]; *p; p++) {
			int oc, ret;

			if (*p == 'w') { now += 1000; continue; }
			if (*p == ',') continue;
			oc = *p == 'r' ? SESS_KEY_REFUSED :
				*p == 's' ? SESS_SERVED : SESS_FAILED;
			ret = hold_note(&h, oc, now);
			printf("%c ret=%d fails=%d refused=%d nholds=%d "
			       "secs=%d until=%ld kind=%s\n", *p, ret,
			       h.fails, h.refused, h.nholds, h.secs,
			       (long)h.until,
			       h.until ? (h.is_refused ? "refused" :
					  "failing") : "-");
		}
		return 0;
	}

	if (argc >= 2 && !strcmp(argv[1], "status")) {
		/* status rSECS fSECS .. | none: mark holds and rewrite the
		 * menu's status file (BLE_KBD_STATUS picks the path) */
		int i = 0;

		memset(holds, 0, sizeof(holds));
		for (int a = 2; a < argc && i < MAX_BONDS; a++) {
			struct kbd_hold *h;

			if (!strcmp(argv[a], "none")) break;
			h = &holds[i];
			memset(h, 0, sizeof(*h));
			h->used = 1;
			h->addr.b[0] = (uint8_t)i;
			h->addr.b[1] = 0xee;
			h->addr.b[2] = 0xfe;
			h->addr.b[3] = 0x45;
			h->addr.b[4] = 0x43;
			h->addr.b[5] = 0xec;
			h->is_refused = argv[a][0] == 'r';
			h->secs = atoi(argv[a] + 1);
			h->until = 1000 + h->secs;
			i++;
		}
		status_write();
		printf("entries=%d\n", i);
		return 0;
	}

	if (argc >= 2 && !strcmp(argv[1], "storefull")) {
		/* a ninth bond must evict the oldest entry (#41) */
		nbonds = 0;
		for (int i = 0; i <= MAX_BONDS; i++) {
			bdaddr_t a = { { 0, 0, 0, 0, 0, (uint8_t)i } };

			bond_new(&a, BDADDR_LE_PUBLIC);
		}
		printf("nbonds=%d\n", nbonds);
		for (int i = 0; i < nbonds; i++) {
			char a[18];

			bdaddr_str(&bonds[i].addr, a);
			printf("%d %s\n", i, a);
		}
		return 0;
	}

	if (argc >= 5 && !strcmp(argv[1], "secok")) {
		struct bt_security sec = { (uint8_t)atoi(argv[2]),
					   (uint8_t)atoi(argv[3]) };

		printf("enough=%d\n", sec_enough(&sec, atoi(argv[4])));
		return 0;
	}

	fprintf(stderr, "usage: %s read LEN MTU [mtureq] | peer HEX.. | "
		"holds SEQ | status rSECS.. | storefull | "
		"secok LEVEL KEYSIZE WANT\n",
		argv[0]);
	return 2;
}
