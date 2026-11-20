#!/bin/sh
# cam-dump -a/-e/-g (#44): the SC202CS shutter/gain helper functions,
# compiled for the host and driven through tests/camdump-ae-harness.
# Needs no board: the same cam-ae.c the package builds is linked here.
set -u

repo_root=$(cd -- "$(dirname -- "$0")/../.." && pwd)
srcdir="$repo_root/linux/buildroot-external/package/cam-dump/src"
harness_src="$repo_root/linux/tests/camdump-ae-harness.c"
tmp=${TMPDIR:-/tmp}/cdp-ae-test.$$
bin="$tmp/harness"
rdir=""
trap 'rm -rf -- "$tmp"; [ -n "$rdir" ] && MSYS_NO_PATHCONV=1 docker exec "${bin#docker:}" rm -rf "$rdir" 2>/dev/null' \
	EXIT HUP INT TERM
mkdir -p "$tmp"

if command -v cc >/dev/null 2>&1; then
	cc -O2 -I"$srcdir" -o "$bin" "$harness_src" "$srcdir/cam-ae.c" ||
		{ echo "compile failed"; exit 1; }
elif command -v gcc >/dev/null 2>&1; then
	gcc -O2 -I"$srcdir" -o "$bin" "$harness_src" "$srcdir/cam-ae.c" ||
		{ echo "compile failed"; exit 1; }
elif command -v docker >/dev/null 2>&1 &&
	docker ps --format '{{.Names}}' 2>/dev/null | grep -q .; then
	ct=$(docker ps --format '{{.Names}}' | head -1)
	rdir="/tmp/cdp-ae-h-$$"
	MSYS_NO_PATHCONV=1 docker exec "$ct" mkdir -p "$rdir" || exit 1
	for f in camdump-ae-harness.c cam-ae.c cam-ae.h; do
		case "$f" in
			*.c)	src="$srcdir/$f"
				[ "$f" = camdump-ae-harness.c ] &&
					src="$harness_src" ;;
			*)	src="$srcdir/$f" ;;
		esac
		docker cp "$src" "$ct:$rdir/$f" || exit 1
	done
	MSYS_NO_PATHCONV=1 docker exec "$ct" \
		cc -O2 -I"$rdir" -o "$rdir/harness" \
		"$rdir/camdump-ae-harness.c" "$rdir/cam-ae.c" ||
		{ echo "compile failed"; exit 1; }
	bin="docker:$ct"
else
	echo "SKIP: no host compiler and no docker container"
	exit 0
fi

run() {
	if [ "${bin#docker:}" != "$bin" ]; then
		MSYS_NO_PATHCONV=1 docker exec -i "${bin#docker:}" \
			"$rdir/harness" "$@"
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

# 1. The shutter split must reproduce the init-table bytes exactly:
#    exp_def 0x3dc -> 0x3e00=0x00 0x3e01=0x3d 0x3e02=0xc0.
is "exp 0x3dc splits like the init table" \
	"$(run regs exp 0x3dc)" "00 3d c0"
is "exp max 1244 splits" "$(run regs exp 1244)" "00 4d c0"

# 2. The DIG-priority gain map, generated instead of embedded:
#    block0 fine sweep only, coarse+analog kick in per the vendor map.
is "gain idx 0"   "$(run regs gain 0)"   "80 00 00"
is "gain idx 31"  "$(run regs gain 31)"  "fc 00 00"
is "gain idx 32"  "$(run regs gain 32)"  "80 01 00"
is "gain idx 64"  "$(run regs gain 64)"  "80 01 01"
is "gain idx 96"  "$(run regs gain 96)"  "80 01 03"
is "gain idx 128" "$(run regs gain 128)" "80 01 07"
is "gain idx 160" "$(run regs gain 160)" "80 01 0f"
is "gain idx 191" "$(run regs gain 191)" "fc 01 0f"
is "gain idx over clamps" "$(run regs gain 300)" "fc 01 0f"

# 3. AE: dark frame at the vendor default -> the x4 scale clamp hits
#    the shutter ceiling and the residue goes to the gain index.
is "dark: shutter maxes, gain takes the rest" \
	"$(run ae 18 110 988 0)" "rc=1 exp=1244 gain=50"
is "dark: second step keeps boosting gain" \
	"$(run ae 18 110 1244 50)" "rc=1 exp=1244 gain=114"
is "dark: both rails clamped -> no move" \
	"$(run ae 1 110 1244 191)" "rc=0 exp=1244 gain=191"

# 4. Bright frame: exposure shortens first, gain untouched.
is "bright: exposure shortens, gain stays" \
	"$(run ae 200 110 988 0)" "rc=1 exp=543 gain=0"
is "bright at shutter min: gain index drops" \
	"$(run ae 200 110 8 50)" "rc=1 exp=8 gain=21"

# 5. Deadband and edge cases.
is "at target: deadband holds" "$(run ae 100 110 988 0)" \
	"rc=0 exp=988 gain=0"
is "black frame still moves" \
	"$(run ae 0 110 988 0)" "rc=1 exp=1244 gain=50"

# 6. The package wires it up: cam-ae.c is linked and cam-dump writes
#    the shutter/gain registers in sensor order.
grep -q 'cam-ae\.c' \
	"$repo_root/linux/buildroot-external/package/cam-dump/cam-dump.mk" &&
	ok "cam-dump.mk links cam-ae.c" || bad "mk missing cam-ae.c"
grep -q 'sccb_write(i2c, 0x3e07' "$srcdir/cam-dump.c" &&
	grep -q 'sccb_write(i2c, 0x3e00' "$srcdir/cam-dump.c" &&
	ok "cam-dump writes shutter 0x3e00-02 and gain 0x3e07/06/09" ||
	bad "register writes missing"

[ "$fails" -eq 0 ] && echo "PASS: $checks checks" ||
	{ echo "FAILED: $fails of $checks checks"; exit 1; }
