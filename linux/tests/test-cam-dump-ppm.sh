#!/bin/sh
# cam-dump -f ppm (#44): the BGGR -> RGB 2x2-binned converter with
# gray-world white balance, compiled for the host and driven through
# tests/camdump-ppm-harness with synthetic frames.  Needs no board:
# the same bggr-ppm.c the package builds is linked here.
set -u

repo_root=$(cd -- "$(dirname -- "$0")/../.." && pwd)
srcdir="$repo_root/linux/buildroot-external/package/cam-dump/src"
harness_src="$repo_root/linux/tests/camdump-ppm-harness.c"
tmp=${TMPDIR:-/tmp}/cdp-test.$$
bin="$tmp/harness"
rdir=""
trap 'rm -rf -- "$tmp"; [ -n "$rdir" ] && MSYS_NO_PATHCONV=1 docker exec "${bin#docker:}" rm -rf "$rdir" 2>/dev/null' \
	EXIT HUP INT TERM
mkdir -p "$tmp"

# Compile the converter + harness for the host: a native compiler if
# one exists, else the build container (this box has no host cc).
if command -v cc >/dev/null 2>&1; then
	cc -O2 -I"$srcdir" -o "$bin" "$harness_src" "$srcdir/bggr-ppm.c" ||
		{ echo "compile failed"; exit 1; }
elif command -v gcc >/dev/null 2>&1; then
	gcc -O2 -I"$srcdir" -o "$bin" "$harness_src" "$srcdir/bggr-ppm.c" ||
		{ echo "compile failed"; exit 1; }
elif command -v docker >/dev/null 2>&1 &&
	docker ps --format '{{.Names}}' 2>/dev/null | grep -q .; then
	ct=$(docker ps --format '{{.Names}}' | head -1)
	rdir="/tmp/cdp-h-$$"
	MSYS_NO_PATHCONV=1 docker exec "$ct" mkdir -p "$rdir" || exit 1
	for f in camdump-ppm-harness.c bggr-ppm.c bggr-ppm.h; do
		case "$f" in
			*.c)	src="$srcdir/$f"
				[ "$f" = camdump-ppm-harness.c ] &&
					src="$harness_src" ;;
			*)	src="$srcdir/$f" ;;
		esac
		docker cp "$src" "$ct:$rdir/$f" || exit 1
	done
	MSYS_NO_PATHCONV=1 docker exec "$ct" \
		cc -O2 -I"$rdir" -o "$rdir/harness" \
		"$rdir/camdump-ppm-harness.c" "$rdir/bggr-ppm.c" ||
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

# PPM output is exactly header (15 B) + 640*360*3 pixel bytes.
PPM_SIZE=$((15 + 640 * 360 * 3))

# 1. A uniform gray frame: means are the level, gains are all 1.0,
#    and every pixel is that gray.
run fill 80 80 80 > "$tmp/gray.raw" || bad "fill gray rc=$?"
run ppm < "$tmp/gray.raw" > "$tmp/gray.ppm" 2> "$tmp/gray.err" || bad "ppm gray rc=$?"
head -c 15 "$tmp/gray.ppm" > "$tmp/hdr.got"
printf 'P6\n640 360\n255\n' > "$tmp/hdr.want"
cmp -s "$tmp/hdr.got" "$tmp/hdr.want" &&
	ok "PPM header is 'P6\\n640 360\\n255\\n'" ||
	bad "header: $(od -An -c "$tmp/hdr.got" | head -2)"
[ "$(wc -c < "$tmp/gray.ppm")" -eq "$PPM_SIZE" ] &&
	ok "PPM size is exactly $PPM_SIZE" ||
	bad "size: $(wc -c < "$tmp/gray.ppm") want $PPM_SIZE"
grep -q 'meanR=80 meanG=80 meanB=80' "$tmp/gray.err" &&
	ok "gray means are 80/80/80" || bad "gray means: $(cat "$tmp/gray.err")"
grep -q 'gainR=1.00 gainB=1.00' "$tmp/gray.err" &&
	ok "gray gains are 1.00/1.00" || bad "gray gains: $(cat "$tmp/gray.err")"
[ "$(run px 0 0 < "$tmp/gray.ppm")" = "80 80 80" ] &&
	ok "gray pixel (0,0) is 80 80 80" ||
	bad "gray px(0,0): $(run px 0 0 < "$tmp/gray.ppm")"
[ "$(run px 359 639 < "$tmp/gray.ppm")" = "80 80 80" ] &&
	ok "gray pixel (359,639) is 80 80 80" ||
	bad "gray px(359,639): $(run px 359 639 < "$tmp/gray.ppm")"

# 2. R sites at twice B: gray-world gain is gR=0.5, gB=1.0, and the
#    balanced output comes out gray.
run fill 128 64 64 > "$tmp/rb.raw" || bad "fill rb rc=$?"
run ppm < "$tmp/rb.raw" > "$tmp/rb.ppm" 2> "$tmp/rb.err" || bad "ppm rb rc=$?"
grep -q 'meanR=128 meanG=64 meanB=64' "$tmp/rb.err" &&
	ok "R=2B means are 128/64/64" || bad "rb means: $(cat "$tmp/rb.err")"
grep -q 'gainR=0.50 gainB=1.00' "$tmp/rb.err" &&
	ok "R=2B gains are 0.50/1.00" || bad "rb gains: $(cat "$tmp/rb.err")"
[ "$(run px 7 9 < "$tmp/rb.ppm")" = "64 64 64" ] &&
	ok "R=2B balanced pixel is gray 64 64 64" ||
	bad "rb px: $(run px 7 9 < "$tmp/rb.ppm")"

# 3. A known 2x2 block -> exact RGB under -W (no scaling):
#    B=30, G1=40, G2=51 -> G=45 (floor), R=60.  Neighbours stay 0.
run block 10 20 30 40 51 60 > "$tmp/blk.raw" || bad "block rc=$?"
run ppm -W < "$tmp/blk.raw" > "$tmp/blk.ppm" 2> "$tmp/blk.err" || bad "ppm -W block rc=$?"
[ "$(run px 10 20 < "$tmp/blk.ppm")" = "60 45 30" ] &&
	ok "known block converts exactly (60 45 30, G floor)" ||
	bad "block px: $(run px 10 20 < "$tmp/blk.ppm")"
[ "$(run px 10 21 < "$tmp/blk.ppm")" = "0 0 0" ] &&
	ok "right neighbour stays black" ||
	bad "neighbour px: $(run px 10 21 < "$tmp/blk.ppm")"
[ "$(run px 0 0 < "$tmp/blk.ppm")" = "0 0 0" ] &&
	ok "far pixel stays black" ||
	bad "far px: $(run px 0 0 < "$tmp/blk.ppm")"

# 4. -W leaves the channel values alone and says so on stderr.
run fill 30 60 120 > "$tmp/w.raw" || bad "fill W rc=$?"
run ppm -W < "$tmp/w.raw" > "$tmp/w.ppm" 2> "$tmp/w.err" || bad "ppm -W rc=$?"
grep -q 'wb: off' "$tmp/w.err" &&
	ok "-W prints 'wb: off'" || bad "-W stderr: $(cat "$tmp/w.err")"
[ "$(run px 100 100 < "$tmp/w.ppm")" = "30 60 120" ] &&
	ok "-W keeps raw channel values (30 60 120)" ||
	bad "-W px: $(run px 100 100 < "$tmp/w.ppm")"

# 5. Clamp edges: a pure-red frame (G and B means 0) gives
#    gR = 0/meanR -> 0.25 and gB = meanG/0 -> 4.0; R 255*0.25 -> 63.
run fill 255 0 0 > "$tmp/red.raw" || bad "fill red rc=$?"
run ppm < "$tmp/red.raw" > "$tmp/red.ppm" 2> "$tmp/red.err" || bad "ppm red rc=$?"
grep -q 'gainR=0.25 gainB=4.00' "$tmp/red.err" &&
	ok "zero means clamp to 0.25/4.00" || bad "red gains: $(cat "$tmp/red.err")"
[ "$(run px 0 0 < "$tmp/red.ppm")" = "63 0 0" ] &&
	ok "clamped R output is 63 0 0" ||
	bad "red px: $(run px 0 0 < "$tmp/red.ppm")"

# 6. The package wires it up: the .mk links the converter and
#    cam-dump calls write_ppm on the last captured frame.
grep -q 'cam-dump\.c \$(@D)/bggr-ppm\.c' \
	"$repo_root/linux/buildroot-external/package/cam-dump/cam-dump.mk" &&
	ok "cam-dump.mk links bggr-ppm.c" || bad "mk missing bggr-ppm.c"
grep -q 'write_ppm(ofd, fb, wb)' "$srcdir/cam-dump.c" &&
	ok "ppm path writes the last captured frame" || bad "no write_ppm call"

[ "$fails" -eq 0 ] && echo "PASS: $checks checks" ||
	{ echo "FAILED: $fails of $checks checks"; exit 1; }
