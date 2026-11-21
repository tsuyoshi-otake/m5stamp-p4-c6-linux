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

[ "$fails" -eq 0 ] && echo "PASS: $checks checks" ||
	{ echo "FAILED: $fails of $checks checks"; exit 1; }
