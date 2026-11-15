#!/bin/sh
# Host-side wifi-qr test (requires gcc and python3 with OpenCV).
# Compiles the same sources that the target build uses with
# -std=gnu11 -Wall -Wextra -Werror, renders every vector in vectors/
# to a PGM, decodes it with OpenCV's QRCodeDetector and compares the
# decoded bytes to the payload the encoder produced.
#
#   sh run-host-test.sh [python3]
set -e
cd "$(dirname "$0")"

PY=${1:-python3}
OUT=${WQR_TEST_OUT:-/tmp/wifi-qr-test}
mkdir -p "$OUT"

# 1) compile the shipped sources exactly like the package build
gcc -std=gnu11 -Wall -Wextra -Werror -O2 -Isrc \
	-o "$OUT/wifi-qr" src/wifi-qr.c src/wqr_payload.c src/qrcodegen.c
# 2) compile the host renderer
gcc -std=gnu11 -Wall -Wextra -Werror -O2 -Isrc \
	-o "$OUT/wqr_pbm" test/wqr_pbm.c src/wqr_payload.c src/qrcodegen.c

# 3) render every vector and let OpenCV check it
fail=0
for v in vectors/*.txt; do
	name=$(basename "$v" .txt)
	"$OUT/wqr_pbm" "$v" "$OUT/$name.pgm" "$OUT/$name.expected"
done
"$PY" - "$OUT" <<'PYEOF'
import glob
import os
import sys

import cv2

outdir = sys.argv[1]
det = cv2.QRCodeDetector()
failed = []
for pgm in sorted(glob.glob(os.path.join(outdir, "*.pgm"))):
    name = os.path.basename(pgm)[:-4]
    expected = open(os.path.join(outdir, name + ".expected"),
                    "r", encoding="utf-8").read().rstrip("\n")
    img = cv2.imread(pgm)
    data, pts, _ = det.detectAndDecode(img)
    ok = pts is not None and data == expected
    print("%s %-14s %s" % ("PASS" if ok else "FAIL", name,
                           "" if ok else "decoded=%r expected=%r" % (data, expected)))
    if not ok:
        failed.append(name)
if failed:
    print("FAILED: %d of %d vectors" % (len(failed), len(glob.glob(os.path.join(outdir, '*.pgm')))))
    sys.exit(1)
print("all vectors decoded correctly")
PYEOF
