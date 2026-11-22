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
 *   setup SRV_MTU MAPLEN
 *       hogp_setup() -- MTU, HID service, characteristics, descriptors,
 *       report map, input-report notifications -- against a HOGP
 *       keyboard (hogp_keyboard) that answers MTU SRV_MTU and holds a
 *       MAPLEN-byte report map.  Prints "write 0017 xx .." per CCCD
 *       write, then one "setup=N mtu=.. reports=.. map=.." line.
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

/* a HOGP keyboard: GAP + HID service, report map, HID info, one input
 * report (CCCD + report reference) and one output report.  Serves the
 * discovery requests a real keyboard answers, at most `srv_mtu` per
 * response (the smaller of both MTUs once the client asked), and
 * prints one "write HHHH xx .." line per Write Request it gets. */
struct fattr {
	uint16_t h, type;
	uint8_t len;
	uint8_t v[5];
};

static const struct fattr hogp_db[] = {
	{ 0x0001, 0x2800, 2, { 0x00, 0x18 } },			/* GAP */
	{ 0x0002, 0x2803, 5, { 0x02, 0x03, 0x00, 0x00, 0x2a } },
	{ 0x0003, 0x2a00, 3, { 'k', 'b', 'd' } },
	{ 0x0010, 0x2800, 2, { 0x12, 0x18 } },			/* HID */
	{ 0x0011, 0x2803, 5, { 0x02, 0x12, 0x00, 0x4b, 0x2a } },	/* map */
	{ 0x0012, 0x2a4b, 0, { 0 } },				/* fmap */
	{ 0x0013, 0x2803, 5, { 0x02, 0x14, 0x00, 0x4a, 0x2a } },	/* info */
	{ 0x0014, 0x2a4a, 4, { 0x11, 0x01, 0x00, 0x02 } },
	{ 0x0015, 0x2803, 5, { 0x12, 0x16, 0x00, 0x4d, 0x2a } },	/* input */
	{ 0x0016, 0x2a4d, 3, { 0, 0, 0 } },
	{ 0x0017, 0x2902, 2, { 0, 0 } },				/* CCCD */
	{ 0x0018, 0x2908, 2, { 0x01, 0x01 } },			/* id 1 in */
	{ 0x0019, 0x2803, 5, { 0x0e, 0x1a, 0x00, 0x4d, 0x2a } },	/* output */
	{ 0x001a, 0x2a4d, 1, { 0 } },
	{ 0x001b, 0x2908, 2, { 0x01, 0x02 } },			/* id 1 out */
};
#define HOGP_DB_N	((int)(sizeof(hogp_db) / sizeof(hogp_db[0])))
#define HOGP_LAST	0x001b

static uint8_t fmap[512];
static int fmaplen;

static void att_err(int fd, uint8_t op, uint16_t h, uint8_t code)
{
	uint8_t e[5] = { ATT_OP_ERROR, op, h & 0xff, h >> 8, code };

	if (write(fd, e, sizeof(e)) < 0)
		_exit(3);
}

static void hogp_keyboard(int fd, int srv_mtu)
{
	uint8_t b[600], r[600];
	int mtu = 23;

	for (;;) {
		ssize_t n = read(fd, b, sizeof(b));
		uint16_t s = n >= 3 ? b[1] | (b[2] << 8) : 0;
		uint16_t e = n >= 5 ? b[3] | (b[4] << 8) : 0;
		uint16_t want = n >= 7 ? b[5] | (b[6] << 8) : 0;
		int o = 0, ilen = 0;

		if (n <= 0)
			_exit(0);
		switch (b[0]) {
		case ATT_OP_MTU_REQ:
			mtu = s < srv_mtu ? s : srv_mtu;
			r[0] = ATT_OP_MTU_RSP;
			r[1] = srv_mtu & 0xff;
			r[2] = srv_mtu >> 8;
			o = 3;
			break;
		case ATT_OP_READ_GROUP_REQ:
			r[o++] = ATT_OP_READ_GROUP_RSP;
			r[o++] = 6;
			for (int i = 0; i < HOGP_DB_N; i++) {
				const struct fattr *a = &hogp_db[i];
				uint16_t end = HOGP_LAST;

				if (a->type != 0x2800 || a->h < s || a->h > e)
					continue;
				for (int j = i + 1; j < HOGP_DB_N; j++)
					if (hogp_db[j].type == 0x2800) {
						end = hogp_db[j].h - 1;
						break;
					}
				if (o + 6 > mtu)
					break;
				r[o++] = a->h & 0xff; r[o++] = a->h >> 8;
				r[o++] = end & 0xff; r[o++] = end >> 8;
				r[o++] = a->v[0]; r[o++] = a->v[1];
			}
			if (o == 2) { att_err(fd, b[0], s, ATT_ERR_NOT_FOUND); continue; }
			break;
		case ATT_OP_READ_TYPE_REQ:
			r[o++] = ATT_OP_READ_TYPE_RSP;
			r[o++] = 0;
			for (int i = 0; i < HOGP_DB_N; i++) {
				const struct fattr *a = &hogp_db[i];

				if (a->type != want || a->h < s || a->h > e)
					continue;
				if (!ilen)
					ilen = 2 + a->len;
				if (2 + a->len != ilen || o + ilen > mtu)
					break;
				r[o++] = a->h & 0xff; r[o++] = a->h >> 8;
				memcpy(r + o, a->v, a->len);
				o += a->len;
			}
			if (o == 2) { att_err(fd, b[0], s, ATT_ERR_NOT_FOUND); continue; }
			r[1] = (uint8_t)ilen;
			break;
		case ATT_OP_FIND_INFO_REQ:
			r[o++] = ATT_OP_FIND_INFO_RSP;
			r[o++] = 1;
			for (int i = 0; i < HOGP_DB_N; i++) {
				const struct fattr *a = &hogp_db[i];

				if (a->h < s || a->h > e)
					continue;
				if (o + 4 > mtu)
					break;
				r[o++] = a->h & 0xff; r[o++] = a->h >> 8;
				r[o++] = a->type & 0xff; r[o++] = a->type >> 8;
			}
			if (o == 2) { att_err(fd, b[0], s, ATT_ERR_NOT_FOUND); continue; }
			break;
		case ATT_OP_READ_REQ:
		case ATT_OP_READ_BLOB_REQ: {
			int off = b[0] == ATT_OP_READ_BLOB_REQ && n >= 5 ? e : 0;
			const uint8_t *v = NULL;
			int len = 0;

			for (int i = 0; i < HOGP_DB_N; i++)
				if (hogp_db[i].h == s) {
					v = s == 0x0012 ? fmap : hogp_db[i].v;
					len = s == 0x0012 ? fmaplen : hogp_db[i].len;
				}
			if (!v) { att_err(fd, b[0], s, 0x01); continue; }
			if (off > len) { att_err(fd, b[0], s, 0x07); continue; }
			r[o++] = b[0] + 1;
			int chunk = len - off;

			if (chunk > mtu - 1)
				chunk = mtu - 1;
			memcpy(r + o, v + off, chunk);
			o += chunk;
			break;
		}
		case ATT_OP_WRITE_REQ:
			put_hex(s == 0x0017 ? "write 0017" : "write other",
				b + 3, (int)n - 3);
			if (s != 0x0017) { att_err(fd, b[0], s, 0x03); continue; }
			r[o++] = ATT_OP_WRITE_RSP;
			break;
		default:
			att_err(fd, b[0], s, 0x06);
			continue;
		}
		if (write(fd, r, o) < 0)
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

	if (argc >= 4 && !strcmp(argv[1], "setup")) {
		/* setup SRV_MTU MAPLEN: hogp_setup() -- the HOGP setup the
		 * daemon runs per session and pair runs on the pairing link
		 * (#41) -- against hogp_keyboard() */
		static struct hogp h;
		int srv_mtu = atoi(argv[2]);

		fmaplen = atoi(argv[3]);
		if (srv_mtu < 23 || srv_mtu > 517 || fmaplen < 1 ||
		    fmaplen > (int)sizeof(fmap))
			return 2;
		for (int i = 0; i < fmaplen; i++)
			fmap[i] = (uint8_t)(i * 5 + 1);

		pid_t pid = fork();

		if (pid < 0)
			return 2;
		if (pid == 0) {
			close(sv[0]);
			hogp_keyboard(sv[1], srv_mtu);
			_exit(0);
		}
		close(sv[1]);
		att_fd = sv[0];
		memset(&h, 0, sizeof(h));

		int sub = hogp_setup(&h, 0);

		close(att_fd);
		waitpid(pid, NULL, 0);
		printf("setup=%d mtu=%u reports=%d map=%d mapok=%d ids=%d "
		       "info=%04x\n", sub, att_mtu, h.nreports, h.map_len,
		       h.map_len == fmaplen && !memcmp(h.report_map, fmap, fmaplen),
		       h.uses_report_ids, h.info_handle);
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

	fprintf(stderr, "usage: %s read LEN MTU [mtureq] | setup SRV_MTU "
		"MAPLEN | peer HEX.. | holds SEQ | status rSECS.. | storefull | "
		"secok LEVEL KEYSIZE WANT\n",
		argv[0]);
	return 2;
}
