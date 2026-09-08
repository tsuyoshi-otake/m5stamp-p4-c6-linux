#!/bin/sh
set -eu

repo_root=$(cd -- "$(dirname -- "$0")/../.." && pwd)
updater="$repo_root/linux/m3-lab/rootfs-overlay/usr/sbin/easystick-update-status"
tmp=${TMPDIR:-/tmp}/easystick-status-test.$$
trap 'rm -rf -- "$tmp"' EXIT HUP INT TERM
mkdir -p "$tmp"

EASYSTICK_RUNTIME_DIR="$tmp" sh "$updater" \
	CONNECTED 192.0.2.10 255.255.255.0 192.0.2.1 test sta

grep -Fqx 'status=CONNECTED' "$tmp/easystick-status.txt"
grep -Fqx 'ip_address=192.0.2.10' "$tmp/easystick-status.txt"
grep -Fqx 'ssh_command=ssh m5@192.0.2.10' "$tmp/easystick-status.txt"
[ -s "$tmp/.last_status" ]

before=$(md5sum "$tmp/easystick-status.txt")
EASYSTICK_RUNTIME_DIR="$tmp" sh "$updater" \
	CONNECTED 192.0.2.10 255.255.255.0 192.0.2.1 test sta
after=$(md5sum "$tmp/easystick-status.txt")
[ "$before" = "$after" ]

echo "PASS: runtime status is atomic and deduplicated"
