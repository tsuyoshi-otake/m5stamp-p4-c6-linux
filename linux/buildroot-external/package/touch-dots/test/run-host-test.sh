#!/bin/sh
# Host-side touch-dots test (requires gcc).  Compiles the shipped source
# with -std=gnu11 -Wall -Wextra -Werror, exactly like the package build.
#
#   sh run-host-test.sh
#
# Skips cleanly when gcc is not installed.
set -e
cd "$(dirname "$0")"

if ! command -v gcc >/dev/null 2>&1; then
	echo "SKIP: gcc not installed"
	exit 0
fi

OUT=${TD_TEST_OUT:-/tmp/touch-dots-test}
mkdir -p "$OUT"

gcc -std=gnu11 -Wall -Wextra -Werror -O2 -Isrc \
	-o "$OUT/touch-dots" src/touch-dots.c

echo "PASS: touch-dots compiles"
