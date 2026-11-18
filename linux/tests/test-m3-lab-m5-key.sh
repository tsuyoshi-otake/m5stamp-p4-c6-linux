#!/bin/sh
# Host test for m5-key's touch->console-cell mapping (#42): all four
# fbcon rotations, corners and midpoints, and taps that land outside
# the framebuffer or outside any menu block.  m5-key's "map" command
# runs the exact tap_to_cell() the binary uses, so no board is needed.
set -u

repo_root=$(cd -- "$(dirname -- "$0")/../.." && pwd)
src="$repo_root/linux/buildroot-external/package/m5-key/src/m5-key.c"
bin=/tmp/m5-key-test-$$

# Compile the helper for the host: a native compiler if one exists,
# else the build container (this box has no host gcc).
if command -v cc >/dev/null 2>&1; then
	cc -O2 -o "$bin" "$src" || { echo "compile failed"; exit 1; }
elif command -v gcc >/dev/null 2>&1; then
	gcc -O2 -o "$bin" "$src" || { echo "compile failed"; exit 1; }
elif command -v docker >/dev/null 2>&1 &&
	docker ps --format '{{.Names}}' 2>/dev/null | grep -q .; then
	ct=$(docker ps --format '{{.Names}}' | head -1)
	docker cp "$src" "$ct":/tmp/m5-key-src.c || exit 1
	MSYS_NO_PATHCONV=1 docker exec "$ct" \
		cc -O2 -o /tmp/m5-key-h /tmp/m5-key-src.c ||
		{ echo "compile failed"; exit 1; }
	bin="docker:$ct"
else
	echo "SKIP: no host compiler and no docker container"
	exit 0
fi

run() { # args -> stdout of the helper; stdin is passed through
	if [ "${bin#docker:}" != "$bin" ]; then
		MSYS_NO_PATHCONV=1 docker exec -i "${bin#docker:}" \
			/tmp/m5-key-h "$@"
	else
		"$bin" "$@"
	fi
}

tmp=/tmp/m5-key-t-$$
mkdir -p "$tmp"

fails=0
# $1 description, $2..$8 map args, $9 "row col" expected (or ! for rc!=0)
check() {
	desc=$1; shift
	want=$1; shift
	got=$(run map "$@" 2>/dev/null); rc=$?
	if [ "$want" = "!" ]; then
		[ "$rc" -ne 0 ] && { echo "ok: $desc"; return; }
		echo "FAIL: $desc (got '$got' rc=0)"; fails=$((fails + 1)); return
	fi
	[ "$rc" -eq 0 ] && [ "$got" = "$want" ] &&
		{ echo "ok: $desc"; return; }
	echo "FAIL: $desc (want '$want' got '$got' rc=$rc)"
	fails=$((fails + 1))
}

# Tab5 geometry: 720x1280 fb, rotate=3, 80x22 console.
check "rot3 console TL <- fb (0,1279)" "0 0"   720 1280 3 80 22 0 1279
check "rot3 console BR <- fb (719,0)"  "21 79" 720 1280 3 80 22 719 0
check "rot3 fb TL -> top-right cell"   "0 79"  720 1280 3 80 22 0 0
check "rot3 fb BR -> bottom-left"      "21 0"  720 1280 3 80 22 719 1279
check "rot3 midpoint"                  "10 40" 720 1280 3 80 22 359 639

check "rot0 identity TL"   "0 0"   720 1280 0 80 22 0 0
check "rot0 identity BR"   "21 79" 720 1280 0 80 22 719 1279
check "rot0 centre"        "10 39" 720 1280 0 80 22 359 639

check "rot2 flips both"    "21 79" 720 1280 2 80 22 0 0
check "rot2 BR -> TL"      "0 0"   720 1280 2 80 22 719 1279

check "rot1 CW fb TL -> bottom-left"  "21 0" 720 1280 1 80 22 0 0
check "rot1 CW fb TR -> TL"           "0 0"  720 1280 1 80 22 719 0
check "rot1 CW fb BL -> BR"           "21 79" 720 1280 1 80 22 0 1279
check "rot1 CW fb BR -> TR"           "0 79" 720 1280 1 80 22 719 1279

# Edges and out-of-range pixels.
check "negative px rejected"    "!" 720 1280 3 80 22 -5 600
check "px beyond fb rejected"   "!" 720 1280 3 80 22 720 600
check "py beyond fb rejected"   "!" 720 1280 3 80 22 100 1280

# A tap on a header row (row < first item top) maps to row 0-2 -- the
# menu hit-test sees no block there and ignores it.
r=$(run map 720 1280 3 80 22 5 1000)
[ "$r" = "0 17" ] && echo "ok: header tap -> row 0 (no block)" ||
	{ echo "FAIL: header tap got '$r'"; fails=$((fails + 1)); }

# --- kmap: one tty byte -> one K token (REVIEW 2 #5).  Enter and
# Backspace get names so a literal '?' is unambiguous; other control
# bytes report nothing (rc 1).
kcheck() { # $1 byte(dec) $2 want token (or ! for ignored)
	got=$(run kmap "$1" 2>/dev/null); rc=$?
	if [ "$2" = "!" ]; then
		[ "$rc" -ne 0 ] && [ -z "$got" ] && echo "ok: byte $1 ignored" ||
			{ echo "FAIL: byte $1 want ignored, got '$got' rc=$rc"
				fails=$((fails + 1)); }
	else
		[ "$rc" -eq 0 ] && [ "$got" = "$2" ] && echo "ok: byte $1 -> $2" ||
			{ echo "FAIL: byte $1 want '$2' got '$got' rc=$rc"
				fails=$((fails + 1)); }
	fi
}
kcheck 13 "K enter"
kcheck 10 "K enter"
kcheck 8  "K bs"
kcheck 127 "K bs"
kcheck 63 "K ?"
kcheck 97 "K a"
kcheck 32 "K  "
kcheck 1  "!"
kcheck 27 "!"

# --- feed: the real event state machine on recorded input_events ----
# Host struct input_event = timeval(16) + u16 type + u16 code + s32.
# A working python3 (the WindowsApps stub only opens the Store).
PY=
for c in "${PYTHON:-}" "py -3" python3 python; do
	[ -n "$c" ] && $c -c 'import struct' 2>/dev/null && { PY=$c; break; }
done
if [ -z "$PY" ]; then
	echo "SKIP: no working python for feed-event tests"
else

gen() { # python writes the event stream for a scenario
	$PY - "$tmp/ev.bin" "$1" <<'PYEOF'
import struct, sys
out, kind = sys.argv[1], sys.argv[2]
EV_SYN, EV_KEY, EV_ABS = 0x00, 0x01, 0x03
BTN_TOUCH, ABS_X, ABS_Y = 0x14a, 0x00, 0x01
MTX, MTY, MTID, SYN = 0x35, 0x36, 0x39, 0x00
recs = []
def ev(sec, usec, t, c, v):
    recs.append(struct.pack("<qqHHi", sec, usec, t, c, v))
if kind == "btn":      # BTN_TOUCH tap at abs(300,600), 100 ms
    ev(0,0,EV_KEY,BTN_TOUCH,1);  ev(0,50000,EV_ABS,ABS_X,300)
    ev(0,60000,EV_ABS,ABS_Y,600); ev(0,70000,EV_SYN,SYN,0)
    ev(0,170000,EV_KEY,BTN_TOUCH,0); ev(0,170000,EV_SYN,SYN,0)
elif kind == "mt":     # MT-B tap at abs(100,200)
    ev(1,0,EV_ABS,MTID,7); ev(1,10000,EV_ABS,MTX,100)
    ev(1,10000,EV_ABS,MTY,200); ev(1,10000,EV_SYN,SYN,0)
    ev(1,200000,EV_ABS,MTID,-1); ev(1,200000,EV_SYN,SYN,0)
elif kind == "short":  # 10 ms contact: jitter, no tap
    ev(2,0,EV_KEY,BTN_TOUCH,1); ev(2,0,EV_ABS,ABS_X,300)
    ev(2,0,EV_ABS,ABS_Y,300); ev(2,0,EV_SYN,SYN,0)
    ev(2,10000,EV_KEY,BTN_TOUCH,0); ev(2,10000,EV_SYN,SYN,0)
elif kind == "drag":   # drag ends at abs(500,900)
    ev(3,0,EV_KEY,BTN_TOUCH,1); ev(3,0,EV_ABS,ABS_X,10)
    ev(3,0,EV_ABS,ABS_Y,20); ev(3,0,EV_SYN,SYN,0)
    for i in range(1,6):
        ev(3,i*30000,EV_ABS,ABS_X,10+i*98)
        ev(3,i*30000,EV_ABS,ABS_Y,20+i*176)
        ev(3,i*30000,EV_SYN,SYN,0)
    ev(3,400000,EV_KEY,BTN_TOUCH,0); ev(3,400000,EV_SYN,SYN,0)
elif kind == "two":    # two complete taps in one read(): first wins
    ev(4,0,EV_KEY,BTN_TOUCH,1); ev(4,0,EV_ABS,ABS_X,300)
    ev(4,0,EV_ABS,ABS_Y,600); ev(4,0,EV_SYN,SYN,0)
    ev(4,100000,EV_KEY,BTN_TOUCH,0); ev(4,100000,EV_SYN,SYN,0)
    ev(4,500000,EV_KEY,BTN_TOUCH,1); ev(4,500000,EV_ABS,ABS_X,10)
    ev(4,500000,EV_ABS,ABS_Y,10); ev(4,500000,EV_SYN,SYN,0)
    ev(4,600000,EV_KEY,BTN_TOUCH,0); ev(4,600000,EV_SYN,SYN,0)
open(out, "wb").write(b"".join(recs))
PYEOF
}

fcase() { # $1 desc, $2 want ("T r c" or !), $3 scenario
	gen "$3" || { echo "FAIL: gen $3"; fails=$((fails + 1)); return; }
	got=$(run feed - 720 1280 3 80 22 719 1279 < "$tmp/ev.bin"); rc=$?
	if [ "$2" = "!" ]; then
		[ "$rc" -ne 0 ] && echo "ok: $1" ||
			{ echo "FAIL: $1 got '$got'"; fails=$((fails + 1)); }
	else
		[ "$got" = "$2" ] && echo "ok: $1" ||
			{ echo "FAIL: $1 want '$2' got '$got'"; fails=$((fails + 1)); }
	fi
}

fcase "BTN_TOUCH protocol tap"      "T 9 42"  btn
fcase "MT protocol B tap"           "T 3 67"  mt
fcase "contact shorter than 30 ms"  "!"       short
fcase "drag reports release point"  "T 15 23" drag
fcase "two taps one read: first"    "T 9 42"  two
fi

rm -rf "$tmp"; [ "${bin#docker:}" = "$bin" ] && rm -f "$bin"
[ "$fails" -eq 0 ] && echo PASS || { echo "FAILED: $fails checks"; exit 1; }
