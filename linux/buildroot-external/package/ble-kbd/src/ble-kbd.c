// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * ble-kbd -- BLE HID-over-GATT keyboard client for the Tab5 (#41).
 *
 * No BlueZ on this NOMMU target, so the tool speaks the kernel
 * interfaces directly:
 *   - mgmt socket (AF_BLUETOOTH/SOCK_RAW/BTPROTO_HCI,
 *     HCI_CHANNEL_CONTROL) for adapter power, discovery, pairing and
 *     bond persistence;
 *   - L2CAP socket on the ATT fixed channel (cid 4) for the GATT
 *     client;
 *   - /dev/uhid to feed HID reports to the input layer.
 *
 * Subcommands:
 *   ble-kbd scan [secs]            list HID-advertising LE devices
 *   ble-kbd pair <addr>            bond a keyboard (io_cap KeyboardDisplay)
 *   ble-kbd list                   list stored bonds
 *   ble-kbd forget <addr>|all      unpair + drop the stored bond
 *   ble-kbd daemon                 connect bonded keyboards, run HOGP -> UHID
 *   ble-kbd mgmt-test              raw mgmt bring-up diagnostic
 *   ble-kbd test-fake <addr>       unencrypted vendor-UUID GATT->UHID test
 */
#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>

/* ---- minimal socket ABI ---- */
#ifndef AF_BLUETOOTH
#define AF_BLUETOOTH	31
#endif
#define BTPROTO_HCI	1
#define BTPROTO_L2CAP	0	/* 3 would be BTPROTO_RFCOMM */
#define SOL_HCI		0
#ifndef SOL_BLUETOOTH
#define SOL_BLUETOOTH	274
#endif
#define HCI_CHANNEL_CONTROL	3
#define BT_SECURITY		4
#define BT_SECURITY_MEDIUM	2
struct bt_security {
	uint8_t level;
	uint8_t key_size;
};

struct sockaddr_hci {
	sa_family_t	hci_family;
	uint16_t	hci_dev;
	uint16_t	hci_channel;
};

typedef struct {
	uint8_t b[6];
} __attribute__((packed)) bdaddr_t;

struct sockaddr_l2 {
	sa_family_t	l2_family;
	uint16_t	l2_psm;
	bdaddr_t	l2_bdaddr;
	uint16_t	l2_cid;
	uint8_t		l2_bdaddr_type;
};
#define BDADDR_BREDR		0x00
#define BDADDR_LE_PUBLIC	0x01
#define BDADDR_LE_RANDOM	0x02
#define ATT_CID			4

/* ---- mgmt ABI (subset of include/net/bluetooth/mgmt.h) ---- */
struct mgmt_hdr {
	uint16_t opcode;
	uint16_t index;
	uint16_t len;
} __attribute__((packed));

struct mgmt_addr_info {
	bdaddr_t bdaddr;
	uint8_t  type;
} __attribute__((packed));
#define MGMT_ADDR_INFO_SIZE 7

#define MGMT_OP_READ_VERSION		0x0001
#define MGMT_OP_READ_INDEX_LIST		0x0003
#define MGMT_OP_READ_INFO		0x0004
#define MGMT_OP_SET_POWERED		0x0005
#define MGMT_OP_SET_BONDABLE		0x0009
#define MGMT_OP_SET_LE			0x000D
#define MGMT_OP_LOAD_LONG_TERM_KEYS	0x0013
#define MGMT_OP_GET_CONNECTIONS		0x0015
#define MGMT_OP_SET_IO_CAPABILITY	0x0018
#define MGMT_OP_PAIR_DEVICE		0x0019
#define MGMT_OP_CANCEL_PAIR_DEVICE	0x001A
#define MGMT_OP_UNPAIR_DEVICE		0x001B
#define MGMT_OP_USER_CONFIRM_REPLY	0x001C
#define MGMT_OP_USER_PASSKEY_REPLY	0x001E
#define MGMT_OP_USER_PASSKEY_NEG_REPLY	0x001F
#define MGMT_OP_START_DISCOVERY		0x0023
#define MGMT_OP_STOP_DISCOVERY		0x0024
#define MGMT_OP_SET_SECURE_CONN		0x002D
#define MGMT_OP_LOAD_IRKS		0x0030
#define MGMT_OP_ADD_DEVICE		0x0033

#define MGMT_EV_CMD_COMPLETE		0x0001
#define MGMT_EV_CMD_STATUS		0x0002
#define MGMT_EV_CONTROLLER_ERROR	0x0003
#define MGMT_EV_INDEX_ADDED		0x0004
#define MGMT_EV_INDEX_REMOVED		0x0005
#define MGMT_EV_NEW_SETTINGS		0x0006
#define MGMT_EV_NEW_LONG_TERM_KEY	0x000A
#define MGMT_EV_DEVICE_CONNECTED	0x000B
#define MGMT_EV_DEVICE_DISCONNECTED	0x000C
#define MGMT_EV_CONNECT_FAILED		0x000D
#define MGMT_EV_USER_CONFIRM_REQUEST	0x000F
#define MGMT_EV_USER_PASSKEY_REQUEST	0x0010
#define MGMT_EV_AUTH_FAILED		0x0011
#define MGMT_EV_DEVICE_FOUND		0x0012
#define MGMT_EV_DISCOVERING		0x0013
#define MGMT_EV_PASSKEY_NOTIFY		0x0017
#define MGMT_EV_NEW_IRK			0x0018
#define MGMT_EV_NEW_CONN_PARAM		0x001C

struct mgmt_rp_read_index_list {
	uint16_t num_controllers;
	uint16_t index[];
} __attribute__((packed));

struct mgmt_rp_read_info {
	bdaddr_t bdaddr;
	uint8_t  version;
	uint16_t manufacturer;
	uint32_t supported_settings;
	uint32_t current_settings;
	uint8_t  dev_class[3];
	uint8_t  name[249];
	uint8_t  short_name[11];
} __attribute__((packed));

struct mgmt_mode { uint8_t val; } __attribute__((packed));

struct mgmt_cp_start_discovery { uint8_t type; } __attribute__((packed));
struct mgmt_cp_stop_discovery  { uint8_t type; } __attribute__((packed));
struct mgmt_cp_set_io_capability { uint8_t io_capability; } __attribute__((packed));

struct mgmt_ev_device_found {
	struct mgmt_addr_info addr;
	int8_t   rssi;
	uint32_t flags;
	uint16_t eir_len;
	uint8_t  eir[];
} __attribute__((packed));

struct mgmt_ev_new_settings { uint32_t val; } __attribute__((packed));

struct mgmt_ev_cmd_complete {
	uint16_t opcode;
	uint8_t  status;
	uint8_t  data[];
} __attribute__((packed));

struct mgmt_ev_cmd_status {
	uint16_t opcode;
	uint8_t  status;
} __attribute__((packed));

struct mgmt_ev_user_confirm_request {
	struct mgmt_addr_info addr;
	uint8_t  confirm_hint;
	uint32_t value;
} __attribute__((packed));

struct mgmt_ev_passkey_notify {
	struct mgmt_addr_info addr;
	uint32_t passkey;
	uint8_t  entered;
} __attribute__((packed));

struct mgmt_ltk_info {
	struct mgmt_addr_info addr;
	uint8_t  type;
	uint8_t  initiator;
	uint8_t  enc_size;
	uint16_t ediv;
	uint64_t rand;
	uint8_t  val[16];
} __attribute__((packed));

struct mgmt_ev_new_long_term_key {
	uint8_t store_hint;
	struct mgmt_ltk_info key;
} __attribute__((packed));

struct mgmt_irk_info {
	struct mgmt_addr_info addr;
	uint8_t  val[16];
} __attribute__((packed));

struct mgmt_ev_new_irk {
	uint8_t  store_hint;
	bdaddr_t rpa;
	struct mgmt_irk_info irk;
} __attribute__((packed));

struct mgmt_cp_pair_device {
	struct mgmt_addr_info addr;
	uint8_t  io_cap;
} __attribute__((packed));

struct mgmt_cp_unpair_device {
	struct mgmt_addr_info addr;
	uint8_t  disconnect;
} __attribute__((packed));

struct mgmt_cp_add_device {
	struct mgmt_addr_info addr;
	uint8_t  action;	/* 1 = background scan + auto-connect */
} __attribute__((packed));

struct mgmt_ev_auth_failed {
	struct mgmt_addr_info addr;
	uint8_t  status;
} __attribute__((packed));

struct mgmt_cp_load_ltks {
	uint16_t key_count;
	struct mgmt_ltk_info keys[];
} __attribute__((packed));

struct mgmt_cp_load_irks {
	uint16_t irk_count;
	struct mgmt_irk_info irks[];
} __attribute__((packed));

struct mgmt_ev_device_connected {
	struct mgmt_addr_info addr;
	uint32_t flags;
	uint16_t eir_len;
	uint8_t  eir[];
} __attribute__((packed));

struct mgmt_ev_device_disconnected {
	struct mgmt_addr_info addr;
	uint8_t  reason;
} __attribute__((packed));

struct mgmt_rp_get_connections {
	uint16_t conn_count;
	struct mgmt_addr_info addr[];
} __attribute__((packed));

struct mgmt_ev_new_conn_param {
	struct mgmt_addr_info addr;
	uint8_t  store_hint;
	uint16_t min_interval;
	uint16_t max_interval;
	uint16_t latency;
	uint16_t timeout;
} __attribute__((packed));

/* ---- HCI constants ---- */
/* mgmt Start/Stop Discovery `type` is an address-type bitmask, NOT an
 * HCI scan type: BIT(BDADDR_LE_PUBLIC)|BIT(BDADDR_LE_RANDOM) = 0x06
 * (kernel DISCOV_TYPE_LE); the old HCI-style 0x02 was rejected with
 * MGMT_STATUS_INVALID_PARAMS (0x0d) on hardware.  Stop Discovery must
 * repeat the same value -- the kernel rejects a type that does not
 * match the active discovery. */
#define MGMT_DISCOV_TYPE_LE	0x06

/* Wire-format guards: sizes are the kernel mgmt.h layouts verbatim
 * (addr_info 7, ltk 36, irk 23; see B32 audit). */
_Static_assert(sizeof(struct mgmt_addr_info) == 7, "addr_info");
_Static_assert(sizeof(struct mgmt_rp_read_info) == 280, "read_info");
_Static_assert(sizeof(struct mgmt_ev_device_found) == 14, "dev_found");
_Static_assert(sizeof(struct mgmt_ev_user_confirm_request) == 12, "ucfm_req");
_Static_assert(sizeof(struct mgmt_ev_passkey_notify) == 12, "pk_notify");
_Static_assert(sizeof(struct mgmt_ltk_info) == 36, "ltk_info");
_Static_assert(sizeof(struct mgmt_ev_new_long_term_key) == 37, "new_ltk");
_Static_assert(sizeof(struct mgmt_irk_info) == 23, "irk_info");
_Static_assert(sizeof(struct mgmt_ev_new_irk) == 30, "new_irk");
_Static_assert(sizeof(struct mgmt_ev_device_connected) == 13, "dev_conn");
_Static_assert(sizeof(struct mgmt_ev_device_disconnected) == 8, "dev_disc");
_Static_assert(sizeof(struct mgmt_ev_new_conn_param) == 16, "conn_param");
_Static_assert(sizeof(struct mgmt_cp_pair_device) == 8, "pair_dev");
_Static_assert(sizeof(struct mgmt_cp_unpair_device) == 8, "unpair_dev");
_Static_assert(sizeof(struct mgmt_cp_add_device) == 8, "add_dev");
_Static_assert(sizeof(struct mgmt_ev_auth_failed) == 8, "auth_fail");
_Static_assert(sizeof(struct mgmt_cp_load_ltks) == 2, "load_ltks");
_Static_assert(sizeof(struct mgmt_cp_load_irks) == 2, "load_irks");
_Static_assert(sizeof(struct mgmt_cp_start_discovery) == 1, "start_disc");
_Static_assert(sizeof(struct mgmt_cp_stop_discovery) == 1, "stop_disc");
_Static_assert(sizeof(struct mgmt_mode) == 1, "mode");
_Static_assert(sizeof(struct mgmt_cp_set_io_capability) == 1, "io_cap");

#define MGMT_SETTING_POWERED	(1U << 0)
#define MGMT_SETTING_LE		(1U << 9)
#define IO_KEYBOARD_DISPLAY	0x04
#define HCI_DEV			0

/* NOMMU bFLT: default stack is 4 KB — big frames must be static. */
static uint8_t rxbuf[4200];
static uint8_t txbuf[sizeof(struct mgmt_hdr) + 640];

/* Real UHID ABI from the toolchain sysroot (uapi).  struct uhid_event is
 * ~4.4 KB — it must never live on a NOMMU bFLT stack, hence the statics
 * below. */
#include <linux/uhid.h>
#ifndef BUS_BLUETOOTH
#define BUS_BLUETOOTH		0x05
#endif
/* Old HID cores may still emit the legacy event. */
#ifndef UHID_OUTPUT_EV
#define UHID_OUTPUT_EV		7
#endif

/* ---- ATT protocol ---- */
#define ATT_OP_ERROR		0x01
#define ATT_OP_MTU_REQ		0x02
#define ATT_OP_MTU_RSP		0x03
#define ATT_OP_FIND_INFO_REQ	0x04
#define ATT_OP_FIND_INFO_RSP	0x05
#define ATT_OP_READ_TYPE_REQ	0x08
#define ATT_OP_READ_TYPE_RSP	0x09
#define ATT_OP_READ_REQ		0x0a
#define ATT_OP_READ_RSP		0x0b
#define ATT_OP_READ_BLOB_REQ	0x0c
#define ATT_OP_READ_BLOB_RSP	0x0d
#define ATT_OP_READ_GROUP_REQ	0x10
#define ATT_OP_READ_GROUP_RSP	0x11
#define ATT_OP_WRITE_REQ	0x12
#define ATT_OP_WRITE_RSP	0x13
#define ATT_OP_WRITE_CMD	0x52
#define ATT_OP_NOTIFY		0x1b
#define ATT_OP_INDICATE		0x1d
#define ATT_OP_CONFIRM		0x1e

#define ATT_ERR_NOT_FOUND	0x0a

/* GATT UUIDs (16-bit) */
#define UUID_PRIMARY_SVC	0x2800
#define UUID_CHAR_DECL		0x2803
#define UUID_CCCD		0x2902
#define UUID_REPORT_REF		0x2908
#define UUID_HID_SVC		0x1812
#define UUID_HID_INFO		0x2a4a
#define UUID_REPORT_MAP		0x2a4b
#define UUID_HID_CTRL		0x2a4c
#define UUID_REPORT		0x2a4d
#define UUID_PROTO_MODE		0x2a4e

/* ---- helpers ---- */

static void bdaddr_str(const bdaddr_t *a, char *out)
{
	snprintf(out, 18, "%02X:%02X:%02X:%02X:%02X:%02X",
		 a->b[5], a->b[4], a->b[3], a->b[2], a->b[1], a->b[0]);
}

static int str_bdaddr(const char *s, bdaddr_t *a)
{
	unsigned int b[6];
	if (sscanf(s, "%x:%x:%x:%x:%x:%x",
		   &b[5], &b[4], &b[3], &b[2], &b[1], &b[0]) != 6)
		return -1;
	for (int i = 0; i < 6; i++) a->b[i] = b[i];
	return 0;
}

/* EIR walking: return pointer/len of field type, or NULL */
static const uint8_t *eir_find(const uint8_t *eir, int eir_len,
			       uint8_t type, int *len)
{
	int i = 0;
	while (i + 1 < eir_len) {
		int l = eir[i];
		if (l == 0 || i + l >= eir_len + 1) break;
		if (eir[i + 1] == type) { *len = l - 1; return eir + i + 2; }
		i += l + 1;
	}
	return NULL;
}

static void eir_name(const uint8_t *eir, int eir_len, char *out, size_t cap)
{
	int l = 0;
	const uint8_t *n = eir_find(eir, eir_len, 0x09, &l);
	if (!n) n = eir_find(eir, eir_len, 0x08, &l);
	if (!n) { out[0] = 0; return; }
	if ((size_t)l >= cap) l = cap - 1;
	memcpy(out, n, l); out[l] = 0;
}

static int eir_uuid16_present(const uint8_t *eir, int eir_len, uint16_t want)
{
	int l = 0;
	const uint8_t *u = eir_find(eir, eir_len, 0x02, &l);
	if (!u) u = eir_find(eir, eir_len, 0x03, &l);
	for (; u && l >= 2; u += 2, l -= 2)
		if ((u[0] | (u[1] << 8)) == want) return 1;
	return 0;
}

static int eir_has_hid(const uint8_t *eir, int eir_len)
{
	return eir_uuid16_present(eir, eir_len, UUID_HID_SVC);
}

static const char *ev_name(uint16_t ev)
{
	switch (ev) {
	case MGMT_EV_CMD_COMPLETE: return "CMD_COMPLETE";
	case MGMT_EV_CMD_STATUS: return "CMD_STATUS";
	case MGMT_EV_CONTROLLER_ERROR: return "CONTROLLER_ERROR";
	case MGMT_EV_INDEX_ADDED: return "INDEX_ADDED";
	case MGMT_EV_INDEX_REMOVED: return "INDEX_REMOVED";
	case MGMT_EV_NEW_SETTINGS: return "NEW_SETTINGS";
	case MGMT_EV_NEW_LONG_TERM_KEY: return "NEW_LTK";
	case MGMT_EV_DEVICE_CONNECTED: return "DEVICE_CONNECTED";
	case MGMT_EV_DEVICE_DISCONNECTED: return "DEVICE_DISCONNECTED";
	case MGMT_EV_CONNECT_FAILED: return "CONNECT_FAILED";
	case MGMT_EV_USER_CONFIRM_REQUEST: return "USER_CONFIRM_REQUEST";
	case MGMT_EV_USER_PASSKEY_REQUEST: return "USER_PASSKEY_REQUEST";
	case MGMT_EV_AUTH_FAILED: return "AUTH_FAILED";
	case MGMT_EV_DEVICE_FOUND: return "DEVICE_FOUND";
	case MGMT_EV_DISCOVERING: return "DISCOVERING";
	case MGMT_EV_PASSKEY_NOTIFY: return "PASSKEY_NOTIFY";
	case MGMT_EV_NEW_IRK: return "NEW_IRK";
	case MGMT_EV_NEW_CONN_PARAM: return "NEW_CONN_PARAM";
	default: return "?";
	}
}

/* ---- mgmt plumbing ---- */

static int mgmt_open(void)
{
	struct sockaddr_hci sa = {0};
	int fd = socket(AF_BLUETOOTH, SOCK_RAW | SOCK_CLOEXEC, BTPROTO_HCI);
	if (fd < 0) return -1;
	sa.hci_family = AF_BLUETOOTH;
	sa.hci_dev = 0xFFFF;
	sa.hci_channel = HCI_CHANNEL_CONTROL;
	if (bind(fd, (struct sockaddr *)&sa, sizeof(sa)) < 0) {
		close(fd);
		return -1;
	}
	return fd;
}

static int mgmt_send(int fd, uint16_t opcode, uint16_t index,
		     const void *param, uint16_t plen)
{
	struct mgmt_hdr *h = (void *)txbuf;
	if (plen > (uint16_t)(sizeof(txbuf) - sizeof(*h))) return -1;
	h->opcode = opcode;
	h->index = index;
	h->len = plen;
	if (plen) memcpy(txbuf + sizeof(*h), param, plen);
	return write(fd, txbuf, sizeof(*h) + plen) < 0 ? -1 : 0;
}

/* Read one mgmt event; returns event opcode or <0/0 timeout. */
static int mgmt_recv(int fd, uint8_t *buf, size_t cap, int timeout_ms)
{
	struct pollfd pfd = { .fd = fd, .events = POLLIN };
	struct mgmt_hdr *h = (void *)buf;
	ssize_t n;
	int r = poll(&pfd, 1, timeout_ms);
	if (r <= 0) return r;
	n = read(fd, buf, cap);
	if (n < (ssize_t)sizeof(*h)) return -1;
	if (n < (ssize_t)(sizeof(*h) + h->len)) return -1;
	return h->opcode;
}

/* adapter up: LE + powered; returns 0 or -1 */
static int adapter_up(int mgmt, int verbose)
{
	struct mgmt_mode on = { .val = 1 };
	uint8_t b[64];
	mgmt_send(mgmt, MGMT_OP_SET_LE, HCI_DEV, &on, 1);
	/* Ask for LE Secure Connections: the CACOE trace showed legacy
	 * Just Works because our side never set the SC bit.  A stack
	 * without SC support answers cmd_status != 0 and pairing just
	 * falls back to legacy, so the reply is not checked. */
	mgmt_send(mgmt, MGMT_OP_SET_SECURE_CONN, HCI_DEV, &on, 1);
	mgmt_send(mgmt, MGMT_OP_SET_POWERED, HCI_DEV, &on, 1);
	for (int k = 0; k < 30; k++) {
		int ev = mgmt_recv(mgmt, b, sizeof(b), 250);
		if (ev <= 0) continue;
		if (ev == MGMT_EV_CMD_COMPLETE) {
			struct mgmt_ev_cmd_complete *c = (void *)(b + 6);
			if (c->opcode == MGMT_OP_SET_POWERED && !c->status)
				return 0;
			if (verbose)
				printf("  cmd %04x st=%02x\n", c->opcode, c->status);
		}
		if (ev == MGMT_EV_CMD_STATUS) {
			struct mgmt_ev_cmd_status *s = (void *)(b + 6);
			if (s->status && verbose)
				printf("  cmdstatus %04x st=%02x\n",
				       s->opcode, s->status);
			if (s->opcode == MGMT_OP_SET_POWERED && s->status)
				return -1;
		}
	}
	return -1;
}

/* ---- scan ---- */
static int cmd_scan(int secs)
{
	uint8_t *buf = rxbuf;
	int fd = mgmt_open();
	if (fd < 0) { perror("mgmt socket"); return 1; }
	if (adapter_up(fd, 1) < 0) {
		fprintf(stderr, "adapter power-on failed\n");
	}

	struct mgmt_cp_start_discovery sd = { .type = MGMT_DISCOV_TYPE_LE };
	if (mgmt_send(fd, MGMT_OP_START_DISCOVERY, HCI_DEV, &sd, 1)) {
		perror("start discovery"); return 1; }
	printf("scanning %d s ...\n", secs);
	fflush(stdout);

	int loops = secs * 4;
	while (loops-- > 0) {
		int ev = mgmt_recv(fd, buf, sizeof(rxbuf), 250);
		if (ev <= 0) continue;
		if (ev != MGMT_EV_DEVICE_FOUND) {
			if (ev == MGMT_EV_CMD_STATUS ||
			    ev == MGMT_EV_CMD_COMPLETE) {
				struct mgmt_ev_cmd_complete *c = (void *)(buf + 6);
				printf("  [%s op=%04x st=%02x]\n",
				       ev_name(ev), c->opcode, c->status);
			}
			continue;
		}
		struct mgmt_ev_device_found *d = (void *)(buf + 6);
		char a[18], name[200] = "";
		bdaddr_str(&d->addr.bdaddr, a);
		eir_name(d->eir, d->eir_len, name, sizeof(name));
		printf("  %s type=%u rssi=%d%s name=%s\n", a, d->addr.type,
		       d->rssi,
		       eir_has_hid(d->eir, d->eir_len) ? " [HID]" : "",
		       name);
		fflush(stdout);
	}
	struct mgmt_cp_stop_discovery sp = { .type = MGMT_DISCOV_TYPE_LE };
	mgmt_send(fd, MGMT_OP_STOP_DISCOVERY, HCI_DEV, &sp, 1);
	close(fd);
	return 0;
}

/* ---- mgmt-test: raw bring-up diagnostic ---- */
static int cmd_mgmt_test(void)
{
	uint8_t *buf = rxbuf;
	int fd = mgmt_open();
	if (fd < 0) { perror("mgmt socket"); return 1; }
	printf("mgmt socket ok\n");

	mgmt_send(fd, MGMT_OP_READ_INDEX_LIST, 0xFFFF, NULL, 0);
	for (;;) {
		int ev = mgmt_recv(fd, buf, sizeof(rxbuf), 1000);
		if (ev <= 0) { printf("no index-list reply ev=%d\n", ev); return 1; }
		if (ev == MGMT_EV_CMD_COMPLETE) {
			struct mgmt_ev_cmd_complete *c = (void *)(buf + 6);
			if (c->opcode != MGMT_OP_READ_INDEX_LIST) continue;
			struct mgmt_rp_read_index_list *l = (void *)c->data;
			printf("controllers: %u", l->num_controllers);
			for (int i = 0; i < l->num_controllers; i++)
				printf(" %u", l->index[i]);
			printf("\n");
			break;
		}
	}

	mgmt_send(fd, MGMT_OP_READ_INFO, HCI_DEV, NULL, 0);
	for (;;) {
		int ev = mgmt_recv(fd, buf, sizeof(rxbuf), 1500);
		if (ev <= 0) { printf("no info reply ev=%d\n", ev); return 1; }
		if (ev == MGMT_EV_CMD_COMPLETE) {
			struct mgmt_ev_cmd_complete *c = (void *)(buf + 6);
			if (c->opcode != MGMT_OP_READ_INFO) continue;
			if (c->status) { printf("read_info status=%02x\n", c->status); return 1; }
			struct mgmt_rp_read_info *i = (void *)c->data;
			char a[18]; bdaddr_str(&i->bdaddr, a);
			printf("info: addr=%s ver=%u mfr=%u supp=%08x cur=%08x name=%s\n",
			       a, i->version, i->manufacturer,
			       i->supported_settings, i->current_settings,
			       i->name);
			break;
		}
		printf("  ev %s\n", ev_name(ev));
	}

	if (adapter_up(fd, 1) == 0)
		printf("adapter powered\n");
	else
		printf("adapter power-on failed\n");
	close(fd);
	return 0;
}

/* ================= GATT client over L2CAP ATT ================= */

static int att_fd = -1;
static uint16_t att_mtu = 23;
static uint8_t att_buf[520];

/* connect L2CAP to ATT fixed channel.
 *
 * sec_level != 0 asks for that security level BEFORE connect(): the
 * blocking connect() then returns once the link is encrypted with the
 * stored LTK, or fails when the keyboard no longer has the bond.  Asked
 * after connect() the kernel answers EINVAL on a link that is already
 * secure enough and otherwise parks the socket unwritable until the
 * encryption is done.  The option value is a struct bt_security: the
 * bare 1-byte level this code used to pass has been rejected with
 * EINVAL since Linux 6.9, so no reconnect was ever encrypted (#41). */
static int att_connect(const bdaddr_t *addr, uint8_t addr_type,
		       int sec_level)
{
	struct sockaddr_l2 sa = {0};
	int fd = socket(AF_BLUETOOTH, SOCK_SEQPACKET, BTPROTO_L2CAP);
	if (fd < 0) return -1;

	/* bind local side first: hci0, LE */
	struct sockaddr_l2 la = {0};
	la.l2_family = AF_BLUETOOTH;
	la.l2_bdaddr_type = BDADDR_LE_PUBLIC;
	la.l2_cid = ATT_CID;
	if (bind(fd, (struct sockaddr *)&la, sizeof(la)) < 0) {
		int e = errno;
		close(fd); errno = e; return -1;
	}

	if (sec_level) {
		struct bt_security sec = { .level = sec_level };
		/* bounds the wait for the encryption inside connect() */
		struct timeval tv = { .tv_sec = 20 };
		if (setsockopt(fd, SOL_BLUETOOTH, BT_SECURITY,
			       &sec, sizeof(sec)) < 0)
			printf("  BT_SECURITY %d refused: %s\n",
			       sec_level, strerror(errno));
		setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));
	}

	sa.l2_family = AF_BLUETOOTH;
	sa.l2_cid = ATT_CID;
	sa.l2_bdaddr = *addr;
	sa.l2_bdaddr_type = addr_type;
	if (connect(fd, (struct sockaddr *)&sa, sizeof(sa)) < 0) {
		int e = errno;
		close(fd); errno = e; return -1;
	}
	return fd;
}

/* The keyboard may act as a GATT client as well (its own MTU exchange,
 * a look for our GAP/GATT services).  We serve no attributes, but a
 * request left unanswered times out on the peer after 30 s and it
 * drops the link.  Returns 1 when the PDU was a peer request or
 * command (handled here), 0 when it is a response/notification for
 * the caller. */
#define ATT_MY_MTU	185
static int att_peer_pdu(const uint8_t *p, int n)
{
	uint8_t op = p[0];
	if (op & 0x40) return 1;		/* command: no answer */
	if (op == ATT_OP_CONFIRM) return 1;
	if (op & 1) return 0;			/* response/notify/indicate */
	if (op == ATT_OP_MTU_REQ) {
		uint8_t r[3] = { ATT_OP_MTU_RSP,
				 ATT_MY_MTU & 0xff, ATT_MY_MTU >> 8 };
		write(att_fd, r, sizeof(r));
		return 1;
	}
	/* discovery requests: nothing there; anything else: unsupported */
	uint8_t e[5] = { ATT_OP_ERROR, op, n > 1 ? p[1] : 0, n > 2 ? p[2] : 0,
			 (op == ATT_OP_FIND_INFO_REQ || op == 0x06 ||
			  op == ATT_OP_READ_TYPE_REQ ||
			  op == ATT_OP_READ_GROUP_REQ) ? ATT_ERR_NOT_FOUND : 0x06 };
	write(att_fd, e, sizeof(e));
	return 1;
}

/* one ATT request -> response. Returns rsp len or <0; opcode in *rop */
static int att_req(const uint8_t *req, int reqlen,
		   uint8_t *rop, uint8_t *rsp, int cap, int timeout_ms)
{
	struct pollfd pfd = { .fd = att_fd, .events = POLLIN };
	if (write(att_fd, req, reqlen) != reqlen) return -1;
	for (;;) {
		int r = poll(&pfd, 1, timeout_ms);
		ssize_t n;
		if (r <= 0) return -2;
		n = read(att_fd, att_buf, sizeof(att_buf));
		if (n <= 0) return -1;
		if (att_buf[0] == ATT_OP_INDICATE) {
			uint8_t c = ATT_OP_CONFIRM;
			write(att_fd, &c, 1);
			continue;
		}
		if (att_buf[0] == ATT_OP_NOTIFY) continue;  /* stray */
		if (att_peer_pdu(att_buf, n)) continue;
		*rop = att_buf[0];
		if (att_buf[0] == ATT_OP_ERROR &&
		    n >= 5 && att_buf[1] == req[0]) {
			if (n - 1 <= cap) memcpy(rsp, att_buf + 1, n - 1);
			return n - 1;  /* caller checks att_buf[4] */
		}
		if (n - 1 > cap) n = cap + 1;
		memcpy(rsp, att_buf + 1, n - 1);
		return n - 1;
	}
}

static int att_errcode(const uint8_t *rsp)
{
	/* rsp = {req_op, handle_lo, handle_hi, err} */
	return rsp[3];
}

/* exchange MTU */
static int att_mtu_req(uint16_t want)
{
	uint8_t req[3] = { ATT_OP_MTU_REQ, want & 0xff, want >> 8 };
	uint8_t op, rsp[8];
	int n = att_req(req, 3, &op, rsp, sizeof(rsp), 2000);
	if (n < 0 || op != ATT_OP_MTU_RSP) return -1;
	att_mtu = rsp[0] | (rsp[1] << 8);
	if (att_mtu > 517) att_mtu = 517;
	return att_mtu;
}

/* Read By Group Type (primary services): uuid 2 or 16 bytes */
static int att_read_group(uint16_t start, uint16_t end,
			  const uint8_t *uuid, int uuidlen,
			  uint8_t *rsp, int cap)
{
	uint8_t req[21];
	uint8_t op;
	req[0] = ATT_OP_READ_GROUP_REQ;
	req[1] = start & 0xff; req[2] = start >> 8;
	req[3] = end & 0xff; req[4] = end >> 8;
	memcpy(req + 5, uuid, uuidlen);
	int n = att_req(req, 5 + uuidlen, &op, rsp, cap, 3000);
	if (n < 0) return n;
	if (op == ATT_OP_ERROR) return -(0x1000 | att_errcode(rsp));
	if (op != ATT_OP_READ_GROUP_RSP) return -1;
	return n;
}

/* Read By Type */
static int att_read_type(uint16_t start, uint16_t end,
			 const uint8_t *uuid, int uuidlen,
			 uint8_t *rsp, int cap)
{
	uint8_t req[21];
	uint8_t op;
	req[0] = ATT_OP_READ_TYPE_REQ;
	req[1] = start & 0xff; req[2] = start >> 8;
	req[3] = end & 0xff; req[4] = end >> 8;
	memcpy(req + 5, uuid, uuidlen);
	int n = att_req(req, 5 + uuidlen, &op, rsp, cap, 3000);
	if (n < 0) return n;
	if (op == ATT_OP_ERROR) return -(0x1000 | att_errcode(rsp));
	if (op != ATT_OP_READ_TYPE_RSP) return -1;
	return n;
}

/* Find Information (descriptors) */
static int att_find_info(uint16_t start, uint16_t end,
			 uint8_t *rsp, int cap)
{
	uint8_t req[5] = { ATT_OP_FIND_INFO_REQ,
		start & 0xff, start >> 8, end & 0xff, end >> 8 };
	uint8_t op;
	int n = att_req(req, 5, &op, rsp, cap, 3000);
	if (n < 0) return n;
	if (op == ATT_OP_ERROR) return -(0x1000 | att_errcode(rsp));
	if (op != ATT_OP_FIND_INFO_RSP) return -1;
	return n;
}

/* Read (blob loop for long values). Returns value length, or <0:
 * -(0x1000 | att error), -2 timeout, -1 link error.
 *
 * Read Request = opcode + 16-bit handle (3 bytes), Read Blob adds a
 * 16-bit offset (5 bytes).  Both went out one byte short until #41,
 * so every real keyboard answered Invalid PDU and no report map or
 * report reference was ever read. */
static int att_read(uint16_t handle, uint8_t *out, int cap)
{
	uint8_t req[3] = { ATT_OP_READ_REQ, handle & 0xff, handle >> 8 };
	uint8_t op;
	/* a response fills the MTU: as large as att_buf, off the stack */
	static uint8_t rsp[sizeof(att_buf)];
	int got = att_req(req, sizeof(req), &op, rsp, sizeof(rsp), 3000);
	if (got < 0) return got;
	if (op == ATT_OP_ERROR) return -(0x1000 | att_errcode(rsp));
	if (op != ATT_OP_READ_RSP) return -1;
	int n = got > cap ? cap : got;
	int tot = n;
	memcpy(out, rsp, n);
	/* blob loop while the response fills the mtu */
	while (got == att_mtu - 1 && tot < cap) {
		uint8_t breq[5] = { ATT_OP_READ_BLOB_REQ,
			handle & 0xff, handle >> 8,
			(uint8_t)(tot & 0xff), (uint8_t)(tot >> 8) };
		got = att_req(breq, sizeof(breq), &op, rsp, sizeof(rsp), 3000);
		if (got <= 0 || op != ATT_OP_READ_BLOB_RSP) break;
		n = tot + got > cap ? cap - tot : got;
		memcpy(out + tot, rsp, n); tot += n;
	}
	return tot;
}

/* Write Request; returns 0 on success */
static int att_write(uint16_t handle, const uint8_t *val, int len)
{
	uint8_t req[64];
	uint8_t op, rsp[8];
	if (len > (int)sizeof(req) - 3 || len > att_mtu - 3) return -1;
	req[0] = ATT_OP_WRITE_REQ;
	req[1] = handle & 0xff; req[2] = handle >> 8;
	memcpy(req + 3, val, len);
	int n = att_req(req, 3 + len, &op, rsp, sizeof(rsp), 3000);
	if (n < 0) return n;
	if (op == ATT_OP_ERROR) return -(0x1000 | att_errcode(rsp));
	return op == ATT_OP_WRITE_RSP ? 0 : -1;
}

/* ---- GATT structure discovery ---- */

struct report_desc {
	uint16_t decl_handle;   /* report characteristic decl */
	uint16_t val_handle;    /* value handle = decl+1 */
	uint16_t cccd_handle;
	uint8_t  report_id;
	uint8_t  report_type;   /* 1=input 2=output 3=feature */
};

struct hogp {
	uint16_t svc_start, svc_end;
	uint16_t info_handle;
	uint16_t map_handle;
	struct report_desc reports[16];
	int nreports;
	uint8_t report_map[512];
	int map_len;
	int uses_report_ids;
};

static const uint8_t uuid_primary[2] = { 0x00, 0x28 };
static const uint8_t uuid_char[2] = { 0x03, 0x28 };
static const uint8_t uuid_hid_svc128[16] = {
	/* test service 4a1b1c2d-3e4f-5a6b-7c8d-9e0f1a2b3c4d */
	0x4d, 0x3c, 0x2b, 0x1a, 0x0f, 0x9e, 0x8d, 0x7c,
	0x6b, 0x5a, 0x4f, 0x3e, 0x2d, 0x1c, 0x1b, 0x4a
};
static int g_vendor_test = 0;

/* find the HID service range; test mode uses the vendor 128-bit uuid */
static int hogp_find_service(struct hogp *h)
{
	uint8_t rsp[256];
	uint16_t start = 1;
	for (;;) {
		int n = att_read_group(start, 0xffff, uuid_primary, 2,
				       rsp, sizeof(rsp));
		if (n < 0) {
			if (n == -(0x1000 | ATT_ERR_NOT_FOUND)) break;
			return n;
		}
		int ilen = rsp[0];
		for (int off = 1; off + ilen <= n; off += ilen) {
			uint16_t h0 = rsp[off] | (rsp[off+1] << 8);
			uint16_t e0 = rsp[off+2] | (rsp[off+3] << 8);
			const uint8_t *v = rsp + off + 4;
			int vl = ilen - 4;
			int match = 0;
			if (!g_vendor_test) {
				if (vl == 2 && v[0] == 0x12 && v[1] == 0x18)
					match = 1;
			} else {
				if (vl == 16 && !memcmp(v, uuid_hid_svc128, 16))
					match = 1;
			}
			if (match) { h->svc_start = h0; h->svc_end = e0; return 0; }
		}
		/* continue from last end handle */
		uint16_t last = rsp[n - ilen + 2] | (rsp[n - ilen + 3] << 8);
		if (last <= start || last >= 0xffff) break;
		start = last + 1;
	}
	return -2;
}

/* characteristic decls in service -> locate HID chars */
static int hogp_find_chars(struct hogp *h)
{
	uint8_t rsp[256];
	uint16_t start = h->svc_start;
	while (start <= h->svc_end) {
		int n = att_read_type(start, h->svc_end, uuid_char, 2,
				      rsp, sizeof(rsp));
		if (n < 0) {
			if (n == -(0x1000 | ATT_ERR_NOT_FOUND)) break;
			return n;
		}
		int ilen = rsp[0];
		uint16_t last = 0;
		for (int off = 1; off + ilen <= n; off += ilen) {
			uint16_t dh = rsp[off] | (rsp[off+1] << 8);
			uint16_t vh = rsp[off+3] | (rsp[off+4] << 8);
			uint16_t uuid = rsp[off+5] | (rsp[off+6] << 8);
			if (uuid == UUID_REPORT_MAP) h->map_handle = vh;
			else if (uuid == UUID_HID_INFO) h->info_handle = vh;
			else if (uuid == UUID_REPORT) {
				struct report_desc *r;
				if (h->nreports < 16) {
					r = &h->reports[h->nreports++];
					memset(r, 0, sizeof(*r));
					r->decl_handle = dh;
					r->val_handle = vh;
				}
			}
			last = dh;
		}
		if (!last || last <= start) break;
		start = last + 1;
	}
	return h->map_handle ? 0 : -2;
}

/* descriptors after each report decl -> CCCD + report reference */
static int hogp_find_descs(struct hogp *h)
{
	uint8_t rsp[256];
	/* sort reports by decl handle for range math */
	for (int i = 0; i < h->nreports - 1; i++)
		for (int j = i + 1; j < h->nreports; j++)
			if (h->reports[j].decl_handle < h->reports[i].decl_handle) {
				struct report_desc t = h->reports[i];
				h->reports[i] = h->reports[j];
				h->reports[j] = t;
			}
	for (int i = 0; i < h->nreports; i++) {
		struct report_desc *r = &h->reports[i];
		uint16_t dstart = r->val_handle + 1;
		uint16_t dend = (i + 1 < h->nreports)
			? h->reports[i + 1].decl_handle - 1 : h->svc_end;
		uint16_t start = dstart;
		while (start <= dend) {
			int n = att_find_info(start, dend, rsp, sizeof(rsp));
			if (n < 0) {
				if (n == -(0x1000 | ATT_ERR_NOT_FOUND)) break;
				return n;
			}
			int fmt = rsp[0];
			int ilen = fmt == 1 ? 4 : 18;
			uint16_t last = 0;
			for (int off = 1; off + ilen <= n; off += ilen) {
				uint16_t dh = rsp[off] | (rsp[off+1] << 8);
				if (fmt != 1) { last = dh; continue; }
				uint16_t uuid = rsp[off+2] | (rsp[off+3] << 8);
				if (uuid == UUID_CCCD) r->cccd_handle = dh;
				else if (uuid == UUID_REPORT_REF) {
					uint8_t v[4];
					if (att_read(dh, v, 2) == 2) {
						r->report_id = v[0];
						r->report_type = v[1];
						if (v[0]) h->uses_report_ids = 1;
					}
				}
				last = dh;
			}
			if (!last || last <= start || last >= dend) break;
			start = last + 1;
		}
	}
	return 0;
}

/* enable notifications on all input reports */
static int hogp_subscribe(struct hogp *h)
{
	uint8_t en[2] = { 1, 0 };
	int sub = 0;
	for (int i = 0; i < h->nreports; i++) {
		struct report_desc *r = &h->reports[i];
		if (r->report_type == 2 || r->report_type == 3) continue;
		if (!r->cccd_handle) continue;
		if (att_write(r->cccd_handle, en, 2) == 0) sub++;
	}
	return sub;
}

/* ---- UHID ---- */
static int uhid_open(void)
{
	return open("/dev/uhid", O_RDWR | O_CLOEXEC);
}

/* struct uhid_event is ~4.4 KB (4096-byte data arrays): never on the
 * NOMMU bFLT stack — statics only. */
static struct uhid_event uhid_ev_w;

static int uhid_create(int fd, const struct hogp *h, const char *name)
{
	struct uhid_event *evp = &uhid_ev_w;
	memset(evp, 0, sizeof(*evp));
	evp->type = UHID_CREATE2;
	snprintf((char *)evp->u.create2.name, 128, "%s", name);
	snprintf((char *)evp->u.create2.phys, 64, "ble-kbd/hogp");
	snprintf((char *)evp->u.create2.uniq, 64, "hogp");
	evp->u.create2.rd_size = h->map_len;
	memcpy(evp->u.create2.rd_data, h->report_map, h->map_len);
	evp->u.create2.bus = BUS_BLUETOOTH;
	evp->u.create2.vendor = 0x0001;
	evp->u.create2.product = 0x0001;
	evp->u.create2.version = 1;
	evp->u.create2.country = 0;
	return write(fd, evp, sizeof(*evp)) < 0 ? -1 : 0;
}

static int uhid_input(int fd, const uint8_t *data, int len)
{
	struct uhid_event *evp = &uhid_ev_w;
	if (len > (int)sizeof(evp->u.input2.data)) return -1;
	memset(evp, 0, sizeof(*evp));
	evp->type = UHID_INPUT2;
	evp->u.input2.size = len;
	memcpy(evp->u.input2.data, data, len);
	int sz = offsetof(struct uhid_event, u.input2.data) + len;
	return write(fd, evp, sz) < 0 ? -1 : 0;
}

static int uhid_destroy(int fd)
{
	struct uhid_event *evp = &uhid_ev_w;
	memset(evp, 0, sizeof(*evp));
	evp->type = UHID_DESTROY;
	return write(fd, evp, offsetof(struct uhid_event, u)) < 0 ? -1 : 0;
}

/* GET_REPORT must always be answered — an unanswered request stalls the
 * HID core ~5 s.  We keep no readable state, so reply err=EIO. */
static void uhid_get_report_reply(int fd, uint32_t id)
{
	struct uhid_event *evp = &uhid_ev_w;
	memset(evp, 0, sizeof(*evp));
	evp->type = UHID_GET_REPORT_REPLY;
	evp->u.get_report_reply.id = id;
	evp->u.get_report_reply.err = EIO;
	write(fd, evp, offsetof(struct uhid_event, u.get_report_reply.data));
}

/* SET_REPORT: forward to the keyboard, then ack so the HID core unblocks. */
static void uhid_set_report_reply(int fd, uint32_t id, uint16_t err)
{
	struct uhid_event *evp = &uhid_ev_w;
	memset(evp, 0, sizeof(*evp));
	evp->type = UHID_SET_REPORT_REPLY;
	evp->u.set_report_reply.id = id;
	evp->u.set_report_reply.err = err;
	write(fd, evp, offsetof(struct uhid_event, u) +
		     sizeof(evp->u.set_report_reply));
}

/* forward LED output reports.  UHID_OUTPUT keeps the report ID in
 * data[0] when the map uses IDs; UHID_SET_REPORT carries it in rnum and
 * the payload is raw.  rid < 0 means "ID is data[0]" (OUTPUT path). */
static void uhid_forward_output(struct hogp *h, const uint8_t *data,
				int len, int rid)
{
	for (int i = 0; i < h->nreports; i++) {
		struct report_desc *r = &h->reports[i];
		if (r->report_type != 2) continue;
		if (h->uses_report_ids) {
			if (rid < 0) {
				if (data[0] != r->report_id) continue;
				att_write(r->val_handle, data + 1, len - 1);
			} else {
				if (rid != r->report_id) continue;
				att_write(r->val_handle, data, len);
			}
			return;
		}
		att_write(r->val_handle, data, len);
		return;
	}
}

/* ---- daemon session: connect -> hogp -> uhid -> pump ---- */

static int hogp_session(const bdaddr_t *addr, uint8_t atype,
			int mgmt, int encrypted)
{
	static struct hogp h;
	memset(&h, 0, sizeof(h));
	int ufd = -1, rc = 1;

	att_fd = att_connect(addr, atype,
			     encrypted ? BT_SECURITY_MEDIUM : 0);
	if (att_fd < 0) {
		printf("  l2cap connect failed: %s%s\n", strerror(errno),
		       encrypted ? " (no encryption -- is the bond still on"
				   " the keyboard?)" : "");
		return 1;
	}
	{
		/* key_size 0 = the link is not encrypted */
		struct bt_security sec = { 0, 0 };
		socklen_t sl = sizeof(sec);
		getsockopt(att_fd, SOL_BLUETOOTH, BT_SECURITY, &sec, &sl);
		printf("  l2cap att connected (enc key size %u)\n",
		       sec.key_size);
	}

	if (att_mtu_req(ATT_MY_MTU) < 0)
		printf("  mtu exchange failed (using %u)\n", att_mtu);
	else
		printf("  att mtu %u\n", att_mtu);

	if (hogp_find_service(&h) < 0) {
		printf("  no %s service\n",
		       g_vendor_test ? "vendor-test" : "HID");
		goto out;
	}
	printf("  hid svc %04x-%04x\n", h.svc_start, h.svc_end);
	if (hogp_find_chars(&h) < 0) { printf("  no HID chars\n"); goto out; }
	hogp_find_descs(&h);
	printf("  reports=%d map=%04x info=%04x ids=%d\n",
	       h.nreports, h.map_handle, h.info_handle, h.uses_report_ids);

	h.map_len = att_read(h.map_handle, h.report_map, sizeof(h.report_map));
	if (h.map_len <= 0) {
		if (h.map_len <= -0x1000)
			printf("  report map read failed: att error 0x%02x\n",
			       (-h.map_len) & 0xff);
		else
			printf("  report map read failed: %s\n",
			       h.map_len == -2 ? "timeout" : "link error");
		/* brief: boot protocol fallback could go here */
		goto out;
	}
	printf("  report map %d bytes\n", h.map_len);

	int sub = hogp_subscribe(&h);
	printf("  subscribed %d input reports\n", sub);

	ufd = uhid_open();
	if (ufd < 0) { printf("  /dev/uhid: %s\n", strerror(errno)); goto out; }
	if (uhid_create(ufd, &h, "BLE keyboard") < 0) {
		printf("  uhid create failed\n"); goto out;
	}
	printf("  uhid device created — typing now goes to the console\n");

	/* pump: ATT notifications -> UHID_INPUT2; UHID_OUTPUT -> write */
	for (;;) {
		struct pollfd pf[2] = {
			{ .fd = att_fd, .events = POLLIN },
			{ .fd = ufd, .events = POLLIN },
		};
		int r = poll(pf, 2, -1);
		if (r <= 0) break;
		if (pf[0].revents & POLLIN) {
			ssize_t n = read(att_fd, att_buf, sizeof(att_buf));
			if (n <= 0) break;
			if (att_buf[0] == ATT_OP_NOTIFY && n >= 3) {
				uint16_t vh = att_buf[1] | (att_buf[2] << 8);
				int hl = n - 3;
				/* prepend report id when the map uses ids */
				if (h.uses_report_ids) {
					for (int i = 0; i < h.nreports; i++) {
						struct report_desc *d = &h.reports[i];
						if (d->val_handle != vh) continue;
						uint8_t w[64];
						w[0] = d->report_id;
						if (hl > 63) hl = 63;
						memcpy(w + 1, att_buf + 3, hl);
						uhid_input(ufd, w, hl + 1);
						break;
					}
				} else {
					uhid_input(ufd, att_buf + 3, hl);
				}
			} else if (att_buf[0] == ATT_OP_INDICATE) {
				uint8_t c = ATT_OP_CONFIRM;
				write(att_fd, &c, 1);
			} else {
				att_peer_pdu(att_buf, n);
			}
		}
		if (pf[0].revents & (POLLERR | POLLHUP)) break;
		if (pf[1].revents & POLLIN) {
			static struct uhid_event ev;
			ssize_t n = read(ufd, &ev, sizeof(ev));
			if (n <= 0) continue;
			if (ev.type == UHID_OUTPUT ||
			    ev.type == __UHID_LEGACY_OUTPUT_EV)
				uhid_forward_output(&h, ev.u.output.data,
						    ev.u.output.size, -1);
			else if (ev.type == UHID_GET_REPORT)
				uhid_get_report_reply(ufd, ev.u.get_report.id);
			else if (ev.type == UHID_SET_REPORT) {
				uhid_forward_output(&h, ev.u.set_report.data,
						    ev.u.set_report.size,
						    ev.u.set_report.rnum);
				uhid_set_report_reply(ufd, ev.u.set_report.id, 0);
			}
		}
	}
	rc = 0;
out:
	if (ufd >= 0) { uhid_destroy(ufd); close(ufd); }
	close(att_fd); att_fd = -1;
	return rc;
}

/* ---- test-fake: connect vendor service, no encryption ---- */
static int cmd_test_fake(const char *addr_str)
{
	bdaddr_t a;
	if (str_bdaddr(addr_str, &a)) { fprintf(stderr, "bad addr\n"); return 2; }
	int mgmt = mgmt_open();
	if (mgmt < 0) { perror("mgmt"); return 1; }
	if (adapter_up(mgmt, 1)) { printf("adapter up failed\n"); }
	g_vendor_test = 1;
	return hogp_session(&a, BDADDR_LE_PUBLIC, mgmt, 0);
}

/* ================= bond store =================
 * /boot is only mounted inside S05tmpfs and m5-config's drive_write;
 * at runtime it is an empty directory on the read-only rootfs, so a
 * store there can never be read or written.  The live store is the
 * staged copy in /tmp (S05tmpfs copies it out of the boot FAT like
 * config.txt); ble-bond-store pushes it back into the image so it
 * survives a reboot.  mode 0600, never logged or echoed.
 */
#define BOND_PATH	"/tmp/ble-bonds.dat"
#define BOND_STORE	"/usr/sbin/ble-bond-store"
#define BOND_MAGIC	0x42424b01	/* "BBK\x01" */
#define MAX_BONDS	8

struct bond_entry {
	bdaddr_t addr;
	uint8_t  type;          /* BDADDR_LE_* */
	uint8_t  have_ltk;
	uint8_t  ltk_type, initiator, enc_size;
	uint16_t ediv;
	uint64_t rand;
	uint8_t  ltk[16];
	uint8_t  have_irk;
	bdaddr_t irk_addr;
	uint8_t  irk[16];
	uint8_t  have_conn;
	uint16_t conn_min, conn_max, conn_lat, conn_to;
};

static struct bond_entry bonds[MAX_BONDS];
static int nbonds;

static const char *bond_path(void);

static int bonds_load(void)
{
	uint8_t hdr[8];
	FILE *f = fopen(bond_path(), "rb");
	nbonds = 0;
	if (!f) return 0;                       /* no store = no bonds */
	if (fread(hdr, 1, 8, f) != 8 ||
	    (hdr[0] | hdr[1] << 8 | hdr[2] << 16 | hdr[3] << 24) != BOND_MAGIC) {
		fclose(f); return -1;
	}
	int n = hdr[4] | (hdr[5] << 8);
	if (n > MAX_BONDS) n = MAX_BONDS;
	if (fread(bonds, sizeof(bonds[0]), n, f) != (size_t)n) {
		fclose(f); return -1;
	}
	nbonds = n;
	fclose(f);
	return 0;
}

static const char *bond_path(void)
{
	const char *p = getenv("BLE_KBD_BONDS");
	return (p && *p) ? p : BOND_PATH;
}

/* last save/persist failure, for "could not be saved: <why>" lines */
static char bond_err[96];

static int bonds_save(void)
{
	uint8_t hdr[8] = { BOND_MAGIC & 0xff, (BOND_MAGIC >> 8) & 0xff,
			   (BOND_MAGIC >> 16) & 0xff, (BOND_MAGIC >> 24) & 0xff,
			   nbonds & 0xff, (nbonds >> 8) & 0xff, 0, 0 };
	FILE *f = fopen(bond_path(), "wb");
	if (!f) {
		snprintf(bond_err, sizeof(bond_err), "%s: %s",
			 bond_path(), strerror(errno));
		return -1;
	}
	int bad = fwrite(hdr, 1, 8, f) != 8 ||
	    (nbonds && fwrite(bonds, sizeof(bonds[0]), nbonds, f) != (size_t)nbonds) ||
	    fflush(f) != 0 || fsync(fileno(f)) != 0;
	if (fclose(f) != 0) bad = 1;
	if (bad) {
		snprintf(bond_err, sizeof(bond_err), "write %s: %s",
			 bond_path(), strerror(errno));
		return -1;
	}
	chmod(bond_path(), 0600);
	return 0;
}

/* push the staged store into the boot FAT image through the same
 * copy -> loop-mount -> atomic-swap -> bootsyncd path m5-config uses
 * (boot-edit --put).  Honors /run/.boot-nosave: the helper exits
 * nonzero and the caller reports the bond as not saved. */
static int bonds_persist(void)
{
	const char *h = getenv("BLE_KBD_BOND_STORE");
	char cmd[256];
	if (!h || !*h) h = BOND_STORE;
	snprintf(cmd, sizeof(cmd), "exec %s", h);
	int rc = system(cmd);
	if (rc == -1 || !WIFEXITED(rc) || WEXITSTATUS(rc) != 0) {
		snprintf(bond_err, sizeof(bond_err),
			 "boot image update failed (rc %d)",
			 rc == -1 ? -1 : WEXITSTATUS(rc));
		return -1;
	}
	return 0;
}

/* staged write + image persist; bond_err holds the reason on -1.
 * BLE_KBD_PERSIST=0 stages the store only and returns 1: the caller
 * (m5-config) then persists it with its own drive_write machinery,
 * which execs one applet at a time from the menu's shell instead of
 * the ble-kbd -> ble-bond-store -> boot-edit chain that NOMMU denies
 * while the menu holds most of the user pool (#44). */
static int bonds_commit(void)
{
	const char *p;
	if (bonds_save()) return -1;
	p = getenv("BLE_KBD_PERSIST");
	if (p && !strcmp(p, "0")) return 1;
	return bonds_persist() == 0 ? 0 : -1;
}

static struct bond_entry *bond_find(const bdaddr_t *a)
{
	for (int i = 0; i < nbonds; i++)
		if (!memcmp(&bonds[i].addr, a, 6)) return &bonds[i];
	return NULL;
}

/* a mgmt_addr_info belongs to a bond when its bdaddr is the stored
 * address or the stored irk (resolved-RPA) address */
static struct bond_entry *bonded_match(const struct mgmt_addr_info *a)
{
	struct bond_entry *b = bond_find(&a->bdaddr);
	for (int i = 0; !b && i < nbonds; i++)
		if (bonds[i].have_irk &&
		    !memcmp(&bonds[i].irk_addr, &a->bdaddr, 6))
			b = &bonds[i];
	return b;
}

static struct bond_entry *bond_new(const bdaddr_t *a, uint8_t type)
{
	struct bond_entry *b = bond_find(a);
	if (b) return b;
	if (nbonds >= MAX_BONDS) return NULL;
	b = &bonds[nbonds++];
	memset(b, 0, sizeof(*b));
	b->addr = *a;
	b->type = type;
	return b;
}

/* ================= pair =================
 * The passkey is rendered in 5-row block digits when stdout is a tty
 * so it cannot be missed on a small panel; it is one-time display
 * only -- never persisted, never logged anywhere else.
 */
static const char *const dig5[10][5] = {
	{ " ### ", "#   #", "#   #", "#   #", " ### " }, /* 0 */
	{ "  #  ", " ##  ", "  #  ", "  #  ", " ### " }, /* 1 */
	{ " ### ", "#   #", "  ## ", " #   ", "#####" }, /* 2 */
	{ "###  ", "   # ", " ##  ", "   # ", "###  " }, /* 3 */
	{ "#  # ", "#  # ", "#####", "   # ", "   # " }, /* 4 */
	{ "#####", "#    ", "#### ", "    #", "#### " }, /* 5 */
	{ " ### ", "#    ", "#### ", "#   #", " ### " }, /* 6 */
	{ "#####", "   # ", "  #  ", " #   ", "#    " }, /* 7 */
	{ " ### ", "#   #", " ### ", "#   #", " ### " }, /* 8 */
	{ " ### ", "#   #", " ####", "    #", " ### " }, /* 9 */
};

static void show_passkey(unsigned pk)
{
	char d[7];
	snprintf(d, sizeof(d), "%06u", pk % 1000000u);
	if (isatty(1)) {
		printf("\033[1m  type this on the keyboard, then Enter:\n");
		for (int r = 0; r < 5; r++) {
			printf("   ");
			for (int i = 0; i < 6; i++)
				printf("%s", dig5[d[i] - '0'][r]);
			printf("\n");
		}
		printf("  %s\033[0m\n", d);
	} else {
		printf("  type %s on the keyboard, then Enter\n", d);
	}
	fflush(stdout);
}

/* Final outcome lines: the menu keys on them (and on the rc). */
static void pair_result(const char *s)
{
	if (isatty(1))
		printf("\033[1m%s\033[0m\n", s);
	else
		printf("%s\n", s);
	fflush(stdout);
}

static int cmd_pair(const char *addr_str)
{
	uint8_t *buf = rxbuf;
	bdaddr_t a;
	if (str_bdaddr(addr_str, &a)) { fprintf(stderr, "bad addr\n"); return 2; }

	bonds_load();
	int mgmt = mgmt_open();
	if (mgmt < 0) { perror("mgmt"); return 1; }
	adapter_up(mgmt, 0);

	struct mgmt_mode on = { .val = 1 };
	struct mgmt_cp_set_io_capability io = { .io_capability = IO_KEYBOARD_DISPLAY };
	mgmt_send(mgmt, MGMT_OP_SET_BONDABLE, HCI_DEV, &on, 1);
	mgmt_send(mgmt, MGMT_OP_SET_IO_CAPABILITY, HCI_DEV, &io, 1);
	usleep(100000);

	struct mgmt_cp_pair_device cp;
	memset(&cp, 0, sizeof(cp));
	cp.addr.bdaddr = a;
	cp.addr.type = BDADDR_LE_PUBLIC;     /* updated on DEVICE_FOUND */
	cp.io_cap = IO_KEYBOARD_DISPLAY;

	/* short discovery first so the kernel knows the addr type
	 * (60 x 250 ms = 15 s) */
	int seen = 0;
	struct mgmt_cp_start_discovery sd = { .type = MGMT_DISCOV_TYPE_LE };
	mgmt_send(mgmt, MGMT_OP_START_DISCOVERY, HCI_DEV, &sd, 1);
	for (int k = 0; k < 60; k++) {
		int ev = mgmt_recv(mgmt, buf, sizeof(rxbuf), 250);
		if (ev == MGMT_EV_DEVICE_FOUND) {
			struct mgmt_ev_device_found *d = (void *)(buf + 6);
			if (!memcmp(&d->addr.bdaddr, &a, 6)) {
				cp.addr.type = d->addr.type;
				seen = 1;
				break;
			}
		}
	}
	struct mgmt_cp_stop_discovery sp = { .type = MGMT_DISCOV_TYPE_LE };
	mgmt_send(mgmt, MGMT_OP_STOP_DISCOVERY, HCI_DEV, &sp, 1);
	usleep(200000);

	mgmt_send(mgmt, MGMT_OP_PAIR_DEVICE, HCI_DEV, &cp, sizeof(cp));
	printf("pairing %s ...\n", addr_str);
	fflush(stdout);

	/* 360 x 250 ms = 90 s: connect + SMP exchange + a person finding
	 * the keyboard and typing 6 digits. */
	int done = -1;
	int auth_status = -1;
	int connected = 0;
	int got_central = 0;
	struct bond_entry *b = NULL;
	for (int k = 0; k < 360 && done < 0; k++) {
		int ev = mgmt_recv(mgmt, buf, sizeof(rxbuf), 250);
		if (ev <= 0) continue;
		struct mgmt_hdr *h = (void *)buf;
		void *d = buf + sizeof(*h);
		switch (ev) {
		case MGMT_EV_DEVICE_CONNECTED: {
			struct mgmt_ev_device_connected *c2 = d;
			if (!memcmp(&c2->addr.bdaddr, &a, 6)) connected = 1;
			break;
		}
		case MGMT_EV_CMD_COMPLETE: {
			struct mgmt_ev_cmd_complete *c = d;
			if (c->opcode == MGMT_OP_PAIR_DEVICE)
				done = c->status ? -2 : 0;
			break;
		}
		case MGMT_EV_CMD_STATUS: {
			struct mgmt_ev_cmd_status *s = d;
			if (s->opcode == MGMT_OP_PAIR_DEVICE && s->status)
				done = -2;
			break;
		}
		case MGMT_EV_USER_CONFIRM_REQUEST: {
			struct mgmt_ev_user_confirm_request *r = d;
			struct mgmt_addr_info rep = r->addr;
			mgmt_send(mgmt, MGMT_OP_USER_CONFIRM_REPLY,
				  HCI_DEV, &rep, sizeof(rep));
			printf("  just-works/confirm accepted\n");
			break;
		}
		case MGMT_EV_PASSKEY_NOTIFY: {
			struct mgmt_ev_passkey_notify *p = d;
			show_passkey(p->passkey);
			break;
		}
		case MGMT_EV_USER_PASSKEY_REQUEST: {
			/* The keyboard asks US to type a code: we can only
			 * display one -- answer with a negative reply and
			 * say so. */
			struct mgmt_addr_info rep =
				*(struct mgmt_addr_info *)d;
			mgmt_send(mgmt, MGMT_OP_USER_PASSKEY_NEG_REPLY,
				  HCI_DEV, &rep, sizeof(rep));
			printf("  keyboard wants us to type a passkey; we can only display one -- declined\n");
			break;
		}
		case MGMT_EV_NEW_LONG_TERM_KEY: {
			struct mgmt_ev_new_long_term_key *k2 = d;
			/* Legacy pairing can report two LTKs: the keyboard's
			 * (initiator=1, the one a central encrypts with) and
			 * the one we handed out for a role swap (initiator=0).
			 * The store holds one: keep the usable one whatever
			 * the order.  A Secure Connections key (type >= 2)
			 * serves both roles. */
			int central = k2->key.initiator || k2->key.type >= 2;
			b = bond_new(&k2->key.addr.bdaddr, k2->key.addr.type);
			if (b && (central || !got_central)) {
				if (central) got_central = 1;
				b->have_ltk = 1;
				b->ltk_type = k2->key.type;
				b->initiator = k2->key.initiator;
				b->enc_size = k2->key.enc_size;
				b->ediv = k2->key.ediv;
				b->rand = k2->key.rand;
				memcpy(b->ltk, k2->key.val, 16);
			}
			break;
		}
		case MGMT_EV_NEW_IRK: {
			struct mgmt_ev_new_irk *k2 = d;
			b = bond_find(&k2->irk.addr.bdaddr);
			if (!b) b = bond_new(&k2->irk.addr.bdaddr,
					     k2->irk.addr.type);
			if (b) {
				b->have_irk = 1;
				b->irk_addr = k2->irk.addr.bdaddr;
				memcpy(b->irk, k2->irk.val, 16);
			}
			break;
		}
		case MGMT_EV_NEW_CONN_PARAM: {
			struct mgmt_ev_new_conn_param *p = d;
			b = bond_find(&p->addr.bdaddr);
			if (!b) b = bond_new(&p->addr.bdaddr, p->addr.type);
			if (b) {
				b->have_conn = 1;
				b->conn_min = p->min_interval;
				b->conn_max = p->max_interval;
				b->conn_lat = p->latency;
				b->conn_to = p->timeout;
			}
			break;
		}
		case MGMT_EV_AUTH_FAILED: {
			struct mgmt_ev_auth_failed *f = d;
			auth_status = f->status;
			done = -2;
			break;
		}
		}
		(void)h;
	}

	if (done == 0 && b && b->have_ltk) {
		int cr = bonds_commit();
		if (cr == 0) {
			pair_result("paired + bonded (keys stored, not shown)");
			return 0;
		}
		if (cr > 0) {
			pair_result("paired + bonded (staged; "
				    "the caller persists it)");
			return 8;	/* menu: drive_write it into the image */
		}
		char msg[160];
		snprintf(msg, sizeof(msg),
			 "paired, but the bond could not be saved: %s",
			 bond_err);
		pair_result(msg);
		return 7;	/* bonded this boot; daemon restart still helps */
	}
	if (auth_status >= 0) {
		char msg[64];
		snprintf(msg, sizeof(msg), "auth failed (status 0x%02x)",
			 auth_status);
		pair_result(msg);
		return 5;
	}
	if (done < 0) {
		/* stop the kernel's pending pair attempt so the next try
		 * starts clean */
		mgmt_send(mgmt, MGMT_OP_CANCEL_PAIR_DEVICE, HCI_DEV,
			  &cp.addr, sizeof(cp.addr));
		if (connected) {
			pair_result("connected but pairing did not finish");
			return 6;
		}
		if (seen) {
			pair_result("keyboard stopped advertising -- "
				    "is it connected to another device?");
			return 4;
		}
		char msg[64];
		snprintf(msg, sizeof(msg),
			 "not found: %s never advertised", addr_str);
		pair_result(msg);
		return 3;
	}
	pair_result(done == 0 ? "pair failed (no LTK received)"
			    : "pair failed");
	return 1;
}

/* ================= list / forget ================= */
static int cmd_list(void)
{
	if (bonds_load() < 0) { printf("bond store unreadable\n"); return 1; }
	if (!nbonds) { printf("no bonded keyboards\n"); return 0; }
	for (int i = 0; i < nbonds; i++) {
		char a[18];
		bdaddr_str(&bonds[i].addr, a);
		printf("%s  type=%u%s%s\n", a, bonds[i].type,
		       bonds[i].have_ltk ? " ltk" : "",
		       bonds[i].have_irk ? " irk" : "");
	}
	return 0;
}

static int cmd_forget(const char *arg)
{
	int mgmt = mgmt_open();
	if (mgmt < 0) { perror("mgmt"); return 1; }
	adapter_up(mgmt, 0);
	bonds_load();

	if (!strcmp(arg, "all")) {
		for (int i = 0; i < nbonds; i++) {
			struct mgmt_cp_unpair_device cp;
			memset(&cp, 0, sizeof(cp));
			cp.addr.bdaddr = bonds[i].addr;
			cp.addr.type = bonds[i].type;
			cp.disconnect = 1;
			mgmt_send(mgmt, MGMT_OP_UNPAIR_DEVICE, HCI_DEV,
				  &cp, sizeof(cp));
			usleep(100000);
		}
		nbonds = 0;
		int cr = bonds_commit();
		if (cr < 0) {
			printf("bonds forgotten, but the store could not be "
			       "saved: %s\n", bond_err);
			return 1;
		}
		printf(cr ? "all bonds forgotten (staged)\n"
			  : "all bonds forgotten\n");
		return cr ? 8 : 0;
	}

	bdaddr_t a;
	if (str_bdaddr(arg, &a)) { fprintf(stderr, "bad addr\n"); return 2; }
	struct bond_entry *b = bond_find(&a);
	struct mgmt_cp_unpair_device cp;
	memset(&cp, 0, sizeof(cp));
	cp.addr.bdaddr = a;
	cp.addr.type = b ? b->type : BDADDR_LE_PUBLIC;
	cp.disconnect = 1;
	mgmt_send(mgmt, MGMT_OP_UNPAIR_DEVICE, HCI_DEV, &cp, sizeof(cp));
	if (b) {
		memmove(b, b + 1,
			(nbonds - 1 - (b - bonds)) * sizeof(*b));
		nbonds--;
		int cr = bonds_commit();
		if (cr < 0) {
			printf("forgot %s, but the store could not be "
			       "saved: %s\n", arg, bond_err);
			return 1;
		}
		if (cr > 0) {
			printf("forgot %s (staged)\n", arg);
			return 8;
		}
	}
	printf("forgot %s\n", arg);
	return 0;
}

/* ================= daemon =================
 * Scan for bonded keyboards; on a matching DEVICE_FOUND, connect and
 * run the HOGP->UHID session; on disconnect go back to scanning.
 *
 * The C6's HCI side can take minutes to answer after boot (esp32_sdio
 * -110 timeouts): adapter_up is retried every 2 s instead of exiting,
 * one log line per wait, no execs (each vfork+exec costs ~487 KB of
 * the user pool).  INDEX_REMOVED/INDEX_ADDED re-run the whole
 * bring-up: the kernel forgets loaded keys and accept-list devices
 * when the index goes away.
 */

/* adapter_up + key load + background-scan arm; returns the bgscan
 * flag for the wait loop */
static int daemon_setup(int mgmt, uint8_t *buf)
{
	if (adapter_up(mgmt, 1)) {
		printf("daemon: adapter not ready; retrying every 2 s\n");
		while (adapter_up(mgmt, 0)) usleep(2000000);
		printf("daemon: adapter up\n");
	}

	/* feed stored keys to the kernel so SMP can encrypt on connect */
	{
		uint8_t *p = txbuf + sizeof(struct mgmt_hdr);
		uint16_t cnt = 0;
		*(uint16_t *)p = 0; p += 2;
		for (int i = 0; i < nbonds; i++) {
			if (!bonds[i].have_ltk) continue;
			struct mgmt_ltk_info *k = (void *)p;
			memset(k, 0, sizeof(*k));
			k->addr.bdaddr = bonds[i].addr;
			k->addr.type = bonds[i].type;
			k->type = bonds[i].ltk_type;
			k->initiator = bonds[i].initiator;
			k->enc_size = bonds[i].enc_size;
			k->ediv = bonds[i].ediv;
			k->rand = bonds[i].rand;
			memcpy(k->val, bonds[i].ltk, 16);
			p += sizeof(*k); cnt++;
		}
		*(uint16_t *)(txbuf + sizeof(struct mgmt_hdr)) = cnt;
		struct mgmt_hdr *h = (void *)txbuf;
		h->opcode = MGMT_OP_LOAD_LONG_TERM_KEYS;
		h->index = HCI_DEV;
		h->len = p - (txbuf + sizeof(struct mgmt_hdr));
		write(mgmt, txbuf, sizeof(*h) + h->len);
	}
	{
		uint8_t *p = txbuf + sizeof(struct mgmt_hdr);
		uint16_t cnt = 0;
		p += 2;
		for (int i = 0; i < nbonds; i++) {
			if (!bonds[i].have_irk) continue;
			struct mgmt_irk_info *k = (void *)p;
			k->addr.bdaddr = bonds[i].irk_addr;
			k->addr.type = bonds[i].type;
			memcpy(k->val, bonds[i].irk, 16);
			p += sizeof(*k); cnt++;
		}
		*(uint16_t *)(txbuf + sizeof(struct mgmt_hdr)) = cnt;
		struct mgmt_hdr *h = (void *)txbuf;
		h->opcode = MGMT_OP_LOAD_IRKS;
		h->index = HCI_DEV;
		h->len = p - (txbuf + sizeof(struct mgmt_hdr));
		write(mgmt, txbuf, sizeof(*h) + h->len);
	}
	/* drain the two command completions so they aren't misparsed later */
	for (int k = 0; k < 40; k++) {
		int ev = mgmt_recv(mgmt, buf, sizeof(rxbuf), 250);
		if (ev == MGMT_EV_CMD_COMPLETE) {
			struct mgmt_ev_cmd_complete *c = (void *)(buf + 6);
			if (c->opcode == MGMT_OP_LOAD_LONG_TERM_KEYS ||
			    c->opcode == MGMT_OP_LOAD_IRKS)
				continue;
		}
		if (ev <= 0) break;
	}
	usleep(200000);

	/* Prefer the kernel's background scan + auto-connect: one
	 * ADD_DEVICE per bond leaves the radio on a low-duty background
	 * scan in the kernel instead of this daemon's continuous mgmt
	 * discovery (the C6 shares one antenna between Wi-Fi and BLE).
	 * Fall back to the discovery loop when the kernel refuses it.
	 *
	 * action=2 (HCI_AUTO_CONN_ALWAYS), as BlueZ uses for HID: for an
	 * LE address action=1 is HCI_AUTO_CONN_DIRECT, which connects
	 * only on directed advertising -- a keyboard that reconnects with
	 * plain ADV_IND (most cheap ones) was never connected (#41). */
	for (int i = 0; i < nbonds; i++) {
		struct mgmt_cp_add_device ad;
		memset(&ad, 0, sizeof(ad));
		ad.addr.bdaddr = bonds[i].addr;
		ad.addr.type = bonds[i].type;
		ad.action = 2;
		mgmt_send(mgmt, MGMT_OP_ADD_DEVICE, HCI_DEV, &ad, sizeof(ad));
	}
	usleep(200000);
	int bgscan = 1;
	for (int k = 0; k < 40; k++) {
		int ev = mgmt_recv(mgmt, buf, sizeof(rxbuf), 250);
		if (ev <= 0) break;
		if (ev == MGMT_EV_CMD_COMPLETE) {
			struct mgmt_ev_cmd_complete *c = (void *)(buf + 6);
			if (c->opcode == MGMT_OP_ADD_DEVICE && c->status)
				bgscan = 0;
		} else if (ev == MGMT_EV_CMD_STATUS) {
			struct mgmt_ev_cmd_status *s = (void *)(buf + 6);
			if (s->opcode == MGMT_OP_ADD_DEVICE && s->status)
				bgscan = 0;
		}
	}
	/* The drains above consume every event: a DEVICE_CONNECTED that
	 * arrived in one of those windows is already gone.  And a
	 * keyboard bonded seconds ago can still hold its pairing link
	 * when the daemon (re)starts.  Ask the kernel who is connected
	 * and serve the bonded ones before entering the wait loop. */
	mgmt_send(mgmt, MGMT_OP_GET_CONNECTIONS, HCI_DEV, NULL, 0);
	for (int k = 0; k < 40; k++) {
		int ev = mgmt_recv(mgmt, buf, sizeof(rxbuf), 250);
		if (ev <= 0) break;
		if (ev != MGMT_EV_CMD_COMPLETE) continue;
		struct mgmt_ev_cmd_complete *c = (void *)(buf + 6);
		if (c->opcode != MGMT_OP_GET_CONNECTIONS || c->status)
			continue;
		struct mgmt_rp_get_connections *r = (void *)c->data;
		struct mgmt_hdr *h = (void *)buf;
		int n = r->conn_count;
		int max = ((int)h->len - (int)sizeof(*c) - 2) /
			  (int)sizeof(struct mgmt_addr_info);
		if (n > max) n = max;
		for (int i = 0; i < n; i++) {
			struct bond_entry *b = bonded_match(&r->addr[i]);
			if (!b) continue;
			char a[18];
			bdaddr_str(&r->addr[i].bdaddr, a);
			printf("daemon: bonded device %s already connected\n", a);
			hogp_session(&b->addr, b->type, mgmt, 1);
		}
		break;
	}
	if (bgscan)
		printf("daemon: %d bond(s), kernel background scan + auto-connect\n",
		       nbonds);
	else
		printf("daemon: %d bond(s), keys loaded, scanning\n", nbonds);
	return bgscan;
}

static int cmd_daemon(void)
{
	uint8_t *buf = rxbuf;
	/* line-buffered, so `ble-kbd daemon >/tmp/log` shows each state
	 * change as it happens instead of a 4 KB block much later */
	setvbuf(stdout, NULL, _IOLBF, 0);
	if (bonds_load() < 0 || !nbonds) {
		printf("no bonds — daemon exits\n");
		return 0;
	}
	int mgmt = mgmt_open();
	if (mgmt < 0) { perror("mgmt"); return 1; }

	struct mgmt_cp_start_discovery sd = { .type = MGMT_DISCOV_TYPE_LE };
	struct mgmt_cp_stop_discovery sp = { .type = MGMT_DISCOV_TYPE_LE };
	for (;;) {
		int bgscan = daemon_setup(mgmt, buf);
		int backoff = 1, reinit = 0;
		while (!reinit) {
			if (!bgscan)
				mgmt_send(mgmt, MGMT_OP_START_DISCOVERY,
					  HCI_DEV, &sd, 1);
			for (;;) {
				int ev = mgmt_recv(mgmt, buf,
						   sizeof(rxbuf), 1000);
				if (ev < 0) break;
				if (ev == 0) continue;
				if (ev == MGMT_EV_INDEX_REMOVED ||
				    ev == MGMT_EV_INDEX_ADDED) {
					printf("daemon: adapter index %s;"
					       " re-arming\n",
					       ev == MGMT_EV_INDEX_ADDED ?
					       "added" : "removed");
					reinit = 1;
					break;
				}
				struct bond_entry *b = NULL;
				struct mgmt_addr_info *da = NULL;
				if (bgscan) {
					if (ev != MGMT_EV_DEVICE_CONNECTED)
						continue;
					struct mgmt_ev_device_connected *d =
						(void *)(buf + 6);
					da = &d->addr;
				} else {
					if (ev != MGMT_EV_DEVICE_FOUND)
						continue;
					struct mgmt_ev_device_found *d =
						(void *)(buf + 6);
					da = &d->addr;
				}
				b = bonded_match(da);
				if (!b) continue;
				char a[18];
				bdaddr_str(&da->bdaddr, a);
				if (bgscan) {
					printf("daemon: bonded device %s "
					       "connected\n", a);
					hogp_session(&b->addr, b->type,
						     mgmt, 1);
					backoff = 1;
					continue;
				}
				printf("daemon: bonded device %s adv -> "
				       "connect\n", a);
				mgmt_send(mgmt, MGMT_OP_STOP_DISCOVERY,
					  HCI_DEV, &sp, 1);
				usleep(200000);
				hogp_session(&b->addr, b->type, mgmt, 1);
				backoff = 1;
				usleep(backoff * 1000000);
				break;   /* back to discovery */
			}
			if (reinit) break;
			/* ev<0: mgmt error — back off and retry */
			usleep(backoff * 1000000);
			if (backoff < 10) backoff += 2;
		}
		/* reinit: the adapter went away or (re)appeared; loop back
		 * to daemon_setup, which waits inside adapter_up's retry
		 * until the index is usable again */
	}
}

/* ---- usage ---- */
static void usage(void)
{
	fprintf(stderr,
		"usage:\n"
		"  ble-kbd scan [secs]       list LE devices ([HID] marks 0x1812)\n"
		"  ble-kbd mgmt-test         adapter bring-up diagnostic\n"
		"  ble-kbd test-fake <addr>  vendor-UUID GATT->UHID test (no enc)\n"
		"  ble-kbd pair <addr>       bond a keyboard\n"
		"  ble-kbd list              list stored bonds\n"
		"  ble-kbd forget <addr>|all\n"
		"  ble-kbd daemon            serve bonded keyboards (HOGP->UHID)\n");
}

int main(int argc, char **argv)
{
	if (argc < 2) { usage(); return 2; }
	if (!strcmp(argv[1], "scan")) return cmd_scan(argc > 2 ? atoi(argv[2]) : 10);
	if (!strcmp(argv[1], "mgmt-test")) return cmd_mgmt_test();
	if (!strcmp(argv[1], "test-fake")) {
		if (argc < 3) { usage(); return 2; }
		return cmd_test_fake(argv[2]);
	}
	if (!strcmp(argv[1], "pair")) {
		if (argc < 3) { usage(); return 2; }
		return cmd_pair(argv[2]);
	}
	if (!strcmp(argv[1], "list")) return cmd_list();
	if (!strcmp(argv[1], "forget")) {
		if (argc < 3) { usage(); return 2; }
		return cmd_forget(argv[2]);
	}
	if (!strcmp(argv[1], "daemon")) return cmd_daemon();
	usage();
	return 2;
}
