#!/bin/sh
# Tab5: kernel messages go to the second virtual console, not over the
# panel's shell on tty1 (a C6 restart printed dozens of esp32_sdio errors
# across the screen; console=tty0 shows every message under the console
# loglevel on the foreground VT unless the kernel is told otherwise).
# The board-side half is one inittab line and one BusyBox applet; this
# checks that they are there, in the right order, and that nothing else
# claims the VT the messages go to.
set -u

repo_root=$(cd -- "$(dirname -- "$0")/../.." && pwd)
inittab="$repo_root/linux/m3-lab/rootfs-overlay-tab5/etc/inittab"
frag="$repo_root/linux/m3-lab/busybox-tab5.fragment"
fails=0
ok() { echo "ok: $1"; }
bad() { echo "FAIL: $1"; fails=$((fails + 1)); }

# 1. The redirect is a sysinit entry for VT 2, at BusyBox's applet path.
grep -qx '::sysinit:/usr/sbin/setlogcons 2' "$inittab" &&
	ok "inittab pins kernel messages to VT 2" ||
	bad "no '::sysinit:/usr/sbin/setlogcons 2' in $inittab"

# 2. It runs before rcS: the esp32_sdio module loads from S40network, and
# its boot-time errors must already go to VT 2.
first=$(grep -n '^::sysinit:' "$inittab" | head -1)
case "$first" in
	*setlogcons*) ok "setlogcons is the first sysinit entry" ;;
	*) bad "first sysinit entry is '$first'" ;;
esac
[ "$(grep -c '^::sysinit:' "$inittab")" -eq 2 ] &&
	grep -qx '::sysinit:/etc/init.d/rcS' "$inittab" &&
	ok "rcS still runs as the second sysinit entry" ||
	bad "sysinit entries: $(grep '^::sysinit:' "$inittab" | tr '\n' ' ')"

# 3. Nothing runs on tty2: a shell or getty there would read the keys
# typed on the log screen, and the panel's own shell stays on tty1.
grep -q '^tty2:' "$inittab" &&
	bad "inittab starts something on tty2" ||
	ok "nothing runs on tty2"
grep -q '^tty1::respawn:-/bin/sh$' "$inittab" &&
	ok "the panel shell is still on tty1" ||
	bad "tty1 shell line changed"

# 4. The applet is built for the Tab5, and no fragment applied after the
# Tab5 one (the C68 profile's) turns it off again.
grep -qx 'CONFIG_SETLOGCONS=y' "$frag" &&
	ok "busybox-tab5.fragment enables setlogcons" ||
	bad "CONFIG_SETLOGCONS=y missing from $frag"
if grep -q 'SETLOGCONS' "$repo_root/linux/c68/busybox.fragment" \
	"$repo_root/linux/m3-lab/busybox.fragment" 2>/dev/null; then
	bad "another BusyBox fragment mentions SETLOGCONS"
else
	ok "no other fragment touches setlogcons"
fi

# 5. The redirect is Tab5-only: the Stamp-P4 has no VT and keeps its
# inittab without it.
if grep -rqs 'setlogcons' "$repo_root/linux/m3-lab/rootfs-overlay/etc/inittab" \
	"$repo_root/linux/m3/rootfs-overlay/etc/inittab" \
	"$repo_root/linux/buildroot-external/board/stamp-p4/rootfs-overlay/etc/inittab"; then
	bad "a shared inittab runs setlogcons"
else
	ok "the shared inittabs leave the kernel console alone"
fi

if [ "$fails" -eq 0 ]; then
	echo "PASS: Tab5 kernel messages on VT 2"
else
	echo "FAIL: $fails checks failed" >&2
	exit 1
fi
