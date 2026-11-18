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

run() { # args -> stdout of the helper
	if [ "${bin#docker:}" != "$bin" ]; then
		MSYS_NO_PATHCONV=1 docker exec "${bin#docker:}" \
			/tmp/m5-key-h "$@"
	else
		"$bin" "$@"
	fi
}

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

rm -f "$bin" 2>/dev/null
[ "$fails" -eq 0 ] && echo PASS || { echo "FAILED: $fails checks"; exit 1; }
