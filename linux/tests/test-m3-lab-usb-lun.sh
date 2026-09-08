#!/bin/sh
set -eu

repo_root=$(cd -- "$(dirname -- "$0")/../.." && pwd)
helper="$repo_root/linux/m3-lab/rootfs-overlay/usr/sbin/easystick-usb-lun"
tmp=${TMPDIR:-/tmp}/easystick-usb-lun-test.$$
trap 'rm -rf -- "$tmp"' EXIT HUP INT TERM

lun0="$tmp/sys/udc0/device/udc/controller/gadget/lun0"
mkdir -p "$lun0"
: > "$lun0/forced_eject"
: > "$lun0/ro"
: > "$tmp/boot.img"
: > "$lun0/file"

run_helper() {
	EASYSTICK_SYS_CLASS_UDC_ROOT="$tmp/sys" \
	EASYSTICK_BOOT_IMG="$tmp/boot.img" \
		sh "$helper" "$@"
}

[ "$(run_helper state)" = "device-owned" ]
[ "$(run_helper reattach)" = "host-owned" ]
[ "$(run_helper state)" = "host-owned" ]
[ "$(cat "$lun0/file")" = "$tmp/boot.img" ]

printf '%s\n' "$tmp/other.img" > "$lun0/file"
if run_helper state >/dev/null 2>&1; then
	echo "unexpected backing file was accepted" >&2
	exit 1
fi

lun1="$tmp/sys/udc1/device/udc/controller/gadget/lun1"
mkdir -p "$lun1"
: > "$lun1/forced_eject"
: > "$lun1/ro"
: > "$lun1/file"
if run_helper state >/dev/null 2>&1; then
	echo "multiple LUNs were accepted" >&2
	exit 1
fi

echo "PASS: legacy mass-storage LUN ownership transitions"
