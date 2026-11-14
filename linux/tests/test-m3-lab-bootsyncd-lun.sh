#!/bin/sh
# bootsyncd lun_state (#11 tab5): no UDC -> "no-drive" without exec'ing
# usb-lun at all; a UDC with no LUN -> "error" through the real helper.
set -u

repo_root=$(cd -- "$(dirname -- "$0")/../.." && pwd)
daemon="$repo_root/linux/m3-lab/rootfs-overlay/usr/sbin/bootsyncd"
helper="$repo_root/linux/m3-lab/rootfs-overlay/usr/sbin/usb-lun"
tmp=${TMPDIR:-/tmp}/bootsyncd-lun-test.$$
trap 'rm -rf -- "$tmp"' EXIT HUP INT TERM
mkdir -p "$tmp/sys"

# Source just the poll functions; globals they use are set below.
sed -n '/^find_lun_file()/,/^}/p;/^udc_present()/,/^}/p;/^lun_state()/,/^}/p' \
	"$daemon" > "$tmp/funcs.sh"

# A LUN_CTL that records every invocation and fails like the real helper
# does when no g_mass_storage LUN exists (exit 2).
stub="$tmp/usb-lun-stub"
cat > "$stub" <<EOF
#!/bin/sh
echo call >> "$tmp/stub-calls"
exit 2
EOF
chmod +x "$stub"

BOOT_IMG="$tmp/boot.img"
: > "$BOOT_IMG"
LUN_FILE=""
LUN_OUT="$tmp/.lunout"
LUN_RETRY=0
LUN_RETRY_TICKS=30
RUN_SYSTEM_CLASS_UDC_ROOT="$tmp/sys"
export RUN_SYSTEM_CLASS_UDC_ROOT

fails=0
check() { # name expected-state
	lun_state
	if [ "$LUN_STATE" = "$2" ]; then
		echo "ok: $1"
	else
		echo "FAIL: $1: expected '$2' got '$LUN_STATE'"
		fails=$((fails + 1))
	fi
}

. "$tmp/funcs.sh"

# 1. No UDC entries: no-drive, and usb-lun must not run (NOMMU exec cost).
LUN_CTL="$stub"
check "no UDC" "no-drive"
if [ -f "$tmp/stub-calls" ]; then
	echo "FAIL: usb-lun was exec'd on a drive-less board"
	fails=$((fails + 1))
fi

# 2. A UDC with no g_mass_storage LUN: the real helper reports the error.
LUN_CTL="$helper"
mkdir -p "$tmp/sys/udc0/device/udc/controller/gadget"
check "UDC without LUN" "error"

# 3. A LUN appears: the builtin file read classifies ownership.
mkdir -p "$tmp/sys/udc0/device/udc/controller/gadget/lun0"
: > "$tmp/sys/udc0/device/udc/controller/gadget/lun0/file"
check "empty LUN file" "device-owned"
printf '%s\n' "$BOOT_IMG" > "$tmp/sys/udc0/device/udc/controller/gadget/lun0/file"
check "attached LUN file" "host-owned"

# 4. The UDC disappears again: no-drive, still no usb-lun exec.
rm -rf "$tmp/sys/udc0"
LUN_CTL="$stub"
LUN_FILE=""
check "UDC removed" "no-drive"
if [ -f "$tmp/stub-calls" ]; then
	echo "FAIL: usb-lun was exec'd after the UDC went away"
	fails=$((fails + 1))
fi

[ "$fails" -eq 0 ] && echo "PASS: bootsyncd lun_state drive-less handling" || exit 1
