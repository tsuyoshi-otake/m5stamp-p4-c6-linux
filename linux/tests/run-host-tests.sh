#!/bin/sh
# Run every host-side test in this directory: the test-*.sh and test-*.py
# files need no board, no toolchain and no vendor checkout.  CI runs this, so
# a new test file is picked up without touching the workflow.
# PYTHON names the interpreter where python3 is not the one to use.
set -u

dir=$(cd -- "$(dirname -- "$0")" && pwd)
cd -- "$dir/../.." || exit 1
log=${TMPDIR:-/tmp}/host-tests.$$
trap 'rm -f -- "$log"' EXIT HUP INT TERM

total=0
failed=""
for t in linux/tests/test-*.sh linux/tests/test-*.py; do
	[ -f "$t" ] || continue
	case "$t" in
		*.sh) runner=sh ;;
		*) runner=${PYTHON:-python3} ;;
	esac
	total=$((total + 1))
	if "$runner" "$t" > "$log" 2>&1; then
		echo "ok   $t"
	else
		echo "FAIL $t"
		sed 's/^/     /' "$log"
		failed="$failed $t"
	fi
done

if [ "$total" -eq 0 ]; then
	echo "no tests found under linux/tests" >&2
	exit 1
fi
if [ -n "$failed" ]; then
	echo "FAILED:$failed"
	exit 1
fi
echo "PASS: $total host tests"
