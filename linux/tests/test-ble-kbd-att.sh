#!/bin/sh
# ble-kbd ATT client framing (#41): Read / Read Blob requests must be
# well-formed, long values must be reassembled at any MTU, and a request
# the keyboard sends as a GATT client must be answered.  Needs no board:
# tests/blekbd-att-harness.c includes the same ble-kbd.c the package
# builds and plays the keyboard over a socketpair.
#
# Background: until #41 Read went out as 2 bytes and Read Blob as 4, so
# every real keyboard answered Invalid PDU, no report map was ever read
# and no UHID device was created -- "paired, but nothing can be typed".
set -u

repo_root=$(cd -- "$(dirname -- "$0")/../.." && pwd)
srcdir="$repo_root/linux/buildroot-external/package/ble-kbd/src"
harness_src="$repo_root/linux/tests/blekbd-att-harness.c"
tmp=${TMPDIR:-/tmp}/blekbd-att-test.$$
bin="$tmp/harness"
rdir=""
trap 'rm -rf -- "$tmp"; [ -n "$rdir" ] && MSYS_NO_PATHCONV=1 docker exec "${bin#docker:}" rm -rf "$rdir" 2>/dev/null' \
	EXIT HUP INT TERM
mkdir -p "$tmp"

# ble-kbd.c is Linux-only (linux/uhid.h, AF_BLUETOOTH): a non-Linux host
# compiles and runs the harness in a running build container instead.
native=""
case "$(uname -s 2>/dev/null)" in
	Linux)	if command -v cc >/dev/null 2>&1; then native=cc
		elif command -v gcc >/dev/null 2>&1; then native=gcc
		fi ;;
esac
if [ -n "$native" ]; then
	"$native" -O2 -Wno-unused-result -I"$srcdir" -o "$bin" "$harness_src" ||
		{ echo "compile failed"; exit 1; }
elif command -v docker >/dev/null 2>&1 &&
	docker ps --format '{{.Names}}' 2>/dev/null | grep -q .; then
	ct=$(docker ps --format '{{.Names}}' | head -1)
	rdir="/tmp/blekbd-att-h-$$"
	MSYS_NO_PATHCONV=1 docker exec "$ct" mkdir -p "$rdir" || exit 1
	docker cp "$harness_src" "$ct:$rdir/blekbd-att-harness.c" || exit 1
	docker cp "$srcdir/ble-kbd.c" "$ct:$rdir/ble-kbd.c" || exit 1
	MSYS_NO_PATHCONV=1 docker exec "$ct" \
		cc -O2 -Wno-unused-result -I"$rdir" -o "$rdir/harness" \
		"$rdir/blekbd-att-harness.c" ||
		{ echo "compile failed"; exit 1; }
	bin="docker:$ct"
else
	echo "SKIP: no Linux host compiler and no docker container"
	exit 0
fi

run() {
	if [ "${bin#docker:}" != "$bin" ]; then
		MSYS_NO_PATHCONV=1 docker exec -i "${bin#docker:}" \
			"$rdir/harness" "$@" | tr -d '\r'
	else
		"$bin" "$@"
	fi
}

fails=0
checks=0
ok() { echo "ok: $1"; checks=$((checks + 1)); }
bad() { echo "FAIL: $1"; fails=$((fails + 1)); checks=$((checks + 1)); }
is() { # name got want
	[ "$2" = "$3" ] && ok "$1" || bad "$1: got '$2' want '$3'"
}

# 1. A short value: exactly one Read Request = opcode + 16-bit handle.
is "short read: 3-byte Read Request, value intact" \
	"$(run read 10 23)" "req 0a 2b 00
read=10 ok=1"

# 2. The CACOE's report map: 122 bytes at the default ATT_MTU 23 is one
#    Read plus five Read Blob requests, each opcode + handle + offset.
is "122-byte report map at MTU 23: Read + 5 well-formed Read Blobs" \
	"$(run read 122 23)" "req 0a 2b 00
req 0c 2b 00 16 00
req 0c 2b 00 2c 00
req 0c 2b 00 42 00
req 0c 2b 00 58 00
req 0c 2b 00 6e 00
read=122 ok=1"

# 3. A length that is an exact multiple of MTU-1 ends with an empty blob.
is "44 bytes at MTU 23: trailing empty blob ends the loop" \
	"$(run read 44 23)" "req 0a 2b 00
req 0c 2b 00 16 00
req 0c 2b 00 2c 00
read=44 ok=1"

# 4. A large MTU: one response carries 184 bytes (the old 64-byte
#    response buffer cut it short and stopped the blob loop).
is "300 bytes at MTU 185: 184 + 116, nothing truncated" \
	"$(run read 300 185)" "req 0a 2b 00
req 0c 2b 00 b8 00
read=300 ok=1"

# 5. The keyboard's own Exchange MTU Request in the middle of our read
#    is answered (03 b9 00 = MTU 185) and the read still completes.
out=$(run read 122 23 mtureq)
case "$out" in
	*"req 03 b9 00"*) ok "keyboard's MTU request answered mid-transaction" ;;
	*) bad "keyboard's MTU request not answered: $out" ;;
esac
case "$out" in
	*"read=122 ok=1") ok "read completes around the keyboard's request" ;;
	*) bad "read broken by the keyboard's request: $out" ;;
esac

# 6. att_peer_pdu: requests get an answer, the rest is left alone.
is "peer Read By Group Type -> Attribute Not Found" \
	"$(run peer 10 01 00 ff ff 00 28)" "handled=1 rsp= 01 10 01 00 0a"
is "peer Read Request -> Request Not Supported" \
	"$(run peer 0a 03 00)" "handled=1 rsp= 01 0a 03 00 06"
is "peer Write Command -> swallowed, no answer" \
	"$(run peer 52 03 00 01)" "handled=1 rsp=none"
is "notification is not a peer request" \
	"$(run peer 1b 2d 00 00 00)" "handled=0 rsp=none"
is "read response is not a peer request" \
	"$(run peer 0b 01 02)" "handled=0 rsp=none"

# 7. Kernel contracts the host cannot exercise, pinned in the source:
#    BT_SECURITY takes a struct bt_security (a bare byte is EINVAL since
#    Linux 6.9) and is set before connect(); bonded keyboards are added
#    with action 2 (HCI_AUTO_CONN_ALWAYS -- the CACOE reconnects with
#    plain ADV_IND, which action 1 never connects).
src="$srcdir/ble-kbd.c"
grep -q 'struct bt_security sec = { \.level = sec_level };' "$src" &&
	! grep -q '&lvl, 1' "$src" &&
	ok "BT_SECURITY passes a struct bt_security" ||
	bad "BT_SECURITY option value is not a struct bt_security"
sec_line=$(grep -n 'BT_SECURITY,$' "$src" | head -1 | cut -d: -f1)
con_line=$(grep -n 'if (connect(fd, (struct sockaddr \*)&sa' "$src" |
	head -1 | cut -d: -f1)
[ -n "$sec_line" ] && [ -n "$con_line" ] && [ "$sec_line" -lt "$con_line" ] &&
	ok "security level is requested before connect()" ||
	bad "BT_SECURITY is not set before connect()"
grep -q 'ad\.action = 2;' "$src" &&
	ok "bonded keyboards auto-connect on undirected advertising" ||
	bad "ADD_DEVICE action is not 2"

# 8. B57: the per-bond hold policy (#41) -- a keyboard that answers
#    Start Encryption with "key missing" gets ECONNABORTED on every
#    auto-connect, several times a second, forever.  hold_note counts
#    three consecutive failures into a hold of 60 s, doubling to a
#    600 s cap; a served session resets everything.  All state is in
#    RAM -- the harness drives the same functions the daemon calls.
is "three key refusals trip a 60 s hold, kind refused" \
	"$(run holds rrr)" "r ret=0 fails=1 refused=1 nholds=0 secs=0 until=0 kind=-
r ret=0 fails=2 refused=2 nholds=0 secs=0 until=0 kind=-
r ret=60 fails=0 refused=0 nholds=1 secs=60 until=100060 kind=refused"
is "mixed streak (refuse, fail, refuse) reports as failing" \
	"$(run holds rfr)" "r ret=0 fails=1 refused=1 nholds=0 secs=0 until=0 kind=-
f ret=0 fails=2 refused=1 nholds=0 secs=0 until=0 kind=-
r ret=60 fails=0 refused=0 nholds=1 secs=60 until=100060 kind=failing"
out=$(run holds rrr,w,rrr,w,rrr,w,rrr,w,rrr)
seq=$(printf '%s\n' "$out" | sed -n 's/^r ret=\([0-9]*\).*/\1/p' | tr '\n' ' ')
is "holds double 60 120 240 480, then cap at 600" \
	"$seq" "0 0 60 0 0 120 0 0 240 0 0 480 0 0 600 "
is "a served session resets the streak and the doubling" \
	"$(run holds rrr,w,rr,s,rrr | tail -1)" \
	"r ret=60 fails=0 refused=0 nholds=1 secs=60 until=101060 kind=refused"
is "outcomes during a hold are not counted" \
	"$(run holds rrrf | tail -1)" \
	"f ret=0 fails=0 refused=0 nholds=1 secs=60 until=100060 kind=refused"

# 9. B57: the status file the menu reads (#41) -- one "ADDR state
#    SECS" line per bond on hold; removed when nothing is held.
#    The file lives in the container when the harness runs there.
runst() {
	if [ "${bin#docker:}" != "$bin" ]; then
		MSYS_NO_PATHCONV=1 docker exec -i \
			-e BLE_KBD_STATUS="$rdir/ble.status" \
			"${bin#docker:}" "$rdir/harness" "$@" | tr -d '\r'
	else
		BLE_KBD_STATUS="$tmp/ble.status" "$bin" "$@"
	fi
}
stcat() {
	if [ "${bin#docker:}" != "$bin" ]; then
		MSYS_NO_PATHCONV=1 docker exec "${bin#docker:}" \
			sh -c "cat '$rdir/ble.status' 2>/dev/null || echo -" |
			tr -d '\r'
	else
		cat "$tmp/ble.status" 2>/dev/null || echo "-"
	fi
}
runst status none > /dev/null
is "no bond on hold: status file is absent" "$(stcat)" "-"
runst status r60 > /dev/null
is "one refused bond: one status line" "$(stcat)" \
	"EC:43:45:FE:EE:00 refused 60"
runst status r60 f120 > /dev/null
is "two held bonds: refused and failing lines" "$(stcat)" \
	"EC:43:45:FE:EE:00 refused 60
EC:43:45:FE:EE:01 failing 120"
runst status none > /dev/null
is "last hold gone: status file is removed again" "$(stcat)" "-"

# 10. B57: the ninth bond must not fail the pair (#41) -- this keyboard
#     re-pairs on a new address every time, so the oldest entry is
#     evicted and pair says so once.
is "ninth bond evicts the oldest, order kept, eviction logged" \
	"$(run storefull)" "store full: forgot the oldest bond ..:00:00
nbonds=8
0 01:00:00:00:00:00
1 02:00:00:00:00:00
2 03:00:00:00:00:00
3 04:00:00:00:00:00
4 05:00:00:00:00:00
5 06:00:00:00:00:00
6 07:00:00:00:00:00
7 08:00:00:00:00:00"

# 11. B57 kernel/menu contracts pinned in the source: the kernel's
#     auto-connect list is trimmed with REMOVE_DEVICE (0x0034) while a
#     bond is on hold, and the menu's view of it is an env-seamed
#     status path -- /run/ble-kbd.status by default.
grep -q 'MGMT_OP_REMOVE_DEVICE.*0x0034' "$src" &&
	ok "held bonds are dropped with REMOVE_DEVICE (0x0034)" ||
	bad "MGMT_OP_REMOVE_DEVICE is not 0x0034"
grep -q 'BLE_KBD_STATUS' "$src" &&
	ok "status file path is env-seamed (BLE_KBD_STATUS)" ||
	bad "no BLE_KBD_STATUS seam"

# 12. No key refresh (#41): the link a pairing leaves behind is already
#     encrypted (short term key).  Asking for a security level on it
#     makes the kernel encrypt it again with the new LTK, and the CACOE
#     then refuses that LTK on every later reconnect.  att_connect()
#     looks first with a channel that asks for nothing and keeps a link
#     that is encrypted well enough; BT_SECURITY_MEDIUM is 2.
is "encrypted pairing link (level 2, 16-byte key) is used as it is" \
	"$(run secok 2 16 2)" "enough=1"
is "unencrypted reconnect link gets the encrypting connect" \
	"$(run secok 1 0 2)" "enough=0"
is "level without a key is not encryption" \
	"$(run secok 2 0 2)" "enough=0"
is "encrypted below the wanted level is not enough" \
	"$(run secok 1 16 2)" "enough=0"
is "a stronger link is enough" \
	"$(run secok 3 16 2)" "enough=1"
probe_line=$(grep -n 'int fd = att_connect_at(addr, addr_type, 0,$' "$src" |
	head -1 | cut -d: -f1)
enc_line=$(grep -n 'return att_connect_at(addr, addr_type, sec_level,$' "$src" |
	head -1 | cut -d: -f1)
[ -n "$probe_line" ] && [ -n "$enc_line" ] && [ "$probe_line" -lt "$enc_line" ] &&
	grep -q 'sec_enough(&sec, sec_level)) {$' "$src" &&
	ok "att_connect looks at the link before it asks for encryption" ||
	bad "att_connect does not probe the link first"
pair_cfg='att_fd = att_connect_at(&b->addr, b->type, 0, ATT_PROBE_TIMEOUT);'
[ "$(grep -v -F "$pair_cfg" "$src" | grep -c 'att_connect_at(')" -eq 3 ] &&
	[ "$(grep -c -F "$pair_cfg" "$src")" -eq 1 ] &&
	ok "sessions cannot reach the encrypting connect directly" ||
	bad "att_connect_at has callers besides att_connect and pair's level-0 look"

# 13. The keyboard is set up on the pairing link (#41): the CACOE went
#     silent -- no advert, no reconnect until switched off and on --
#     whenever the host let the pairing link drop untouched, and the
#     menu saves the bond before
#     the daemon starts, longer than the kernel keeps an idle link.  So
#     pair runs the HOGP setup itself (hogp_setup, shared with every
#     daemon session) on that link.  The harness plays a keyboard with
#     an input report (CCCD 0x0017) and an output report (no CCCD).
out=$(run setup 23 122)
is "HOGP setup at MTU 23: one input report on, 122-byte map whole" \
	"$out" "write 0017 01 00
setup=1 mtu=23 reports=2 map=122 mapok=1 ids=1 info=0014"
#     A keyboard that answers a larger MTU than ours: the link uses ours
#     (185), and the report map still arrives whole (taking 247 stopped
#     the blob loop after the first 184 bytes).
is "keyboard answers MTU 247: 185 used, 300-byte map whole" \
	"$(run setup 247 300 | tail -1)" \
	"setup=1 mtu=185 reports=2 map=300 mapok=1 ids=1 info=0014"
#     Source contracts: pair sets the keyboard up before it stores the
#     bond, only on a link that is already encrypted (level 0 look, no
#     key refresh), and the daemon's sessions run the same setup.
cfg_line=$(grep -n 'int sub = pair_configure(b);' "$src" | head -1 | cut -d: -f1)
com_line=$(grep -n 'int cr = bonds_commit();' "$src" | head -1 | cut -d: -f1)
[ -n "$cfg_line" ] && [ -n "$com_line" ] && [ "$cfg_line" -lt "$com_line" ] &&
	ok "pair sets the keyboard up on the pairing link before storing" ||
	bad "pair does not run pair_configure before bonds_commit"
awk '/^static int pair_configure\(/,/^}/' "$src" |
	grep -q '!sec_enough(&sec, BT_SECURITY_MEDIUM)' &&
	ok "pair's setup needs an encrypted link and never asks for one" ||
	bad "pair_configure does not check the link's encryption"
grep -q 'if (hogp_setup(&h, 1) < 0)' "$src" &&
	ok "daemon sessions run the same HOGP setup" ||
	bad "hogp_session does not use hogp_setup"

[ "$fails" -eq 0 ] && echo "PASS: $checks checks" ||
	{ echo "FAILED: $fails of $checks checks"; exit 1; }
