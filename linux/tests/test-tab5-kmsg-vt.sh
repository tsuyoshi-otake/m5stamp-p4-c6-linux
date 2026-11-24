#!/bin/sh
# Tab5: kernel messages go to the second virtual console, not over the
# panel's shell on tty1 (a C6 restart printed dozens of esp32_sdio errors
# across the screen; console=tty0 shows every message under the console
# loglevel on the foreground VT unless the kernel is told otherwise).
# The board-side half is two inittab lines (setlogcons, and m5-key
# vt-keys so that AltGr reaches VT 2 too) and one BusyBox applet; this
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
# its boot-time errors must already go to VT 2.  The AltGr console keys
# come next (2b), then rcS.
sysinit=$(grep '^::sysinit:' "$inittab" | tr '\n' '|')
[ "$sysinit" = '::sysinit:/usr/sbin/setlogcons 2|::sysinit:/usr/bin/m5-key vt-keys|::sysinit:/etc/init.d/rcS|' ] &&
	ok "sysinit runs setlogcons, then m5-key vt-keys, then rcS" ||
	bad "sysinit entries: $sysinit"

# 2b. VT 2 can be reached without a left Alt: the CACOE case keyboard in
# Windows mode sends Win and AltGr and has no F-row.  m5-key vt-keys
# copies the Alt plane's Left/Right entries (Decr/Incr_Console) into the
# AltGr plane, and m5-key is built for every Tab5 image.
m5key="$repo_root/linux/buildroot-external/package/m5-key/src/m5-key.c"
if grep -q 'e.kb_table = 1 << KG_ALT;' "$m5key" &&
	grep -q 'e.kb_table = 1 << KG_ALTGR;' "$m5key" &&
	grep -q 'keys\[\] = { KEY_LEFT, KEY_RIGHT }' "$m5key" &&
	grep -q 'strcmp(argv\[1\], "vt-keys")' "$m5key"; then
	ok "m5-key vt-keys copies Alt+Left/Right into the AltGr plane"
else
	bad "m5-key.c lost vt-keys or its Alt -> AltGr copy"
fi
grep -q 'set_config_value "${br_output}/.config" BR2_PACKAGE_M5_KEY y' \
	"$repo_root/linux/build-m1.sh" &&
	ok "the Tab5 build enables m5-key" ||
	bad "build-m1.sh no longer enables BR2_PACKAGE_M5_KEY"

# 2c. The kernel's own boot messages show on the panel (and the serial
# console) up to KERN_INFO, as on a Raspberry Pi: loglevel=7 in the Tab5
# bootargs.  Runtime console traffic starves SDIO, so S40network must
# still lower the console (dmesg -n 4) before it loads the Wi-Fi module.
dts="$repo_root/linux/dts/esp32p4-tab5.dts"
grep -q 'bootargs = ".* console=tty0 console=ttyGS1,115200n8 loglevel=7 ' "$dts" &&
	ok "Tab5 bootargs print the boot messages (loglevel=7)" ||
	bad "Tab5 bootargs lost loglevel=7 or the tty0/ttyGS1 consoles"
s40="$repo_root/linux/m3-lab/rootfs-overlay/etc/init.d/S40network"
lower=$(grep -n 'dmesg -n 4' "$s40" | head -1 | cut -d: -f1)
load=$(grep -n 'if insmod "$KO"' "$s40" | head -1 | cut -d: -f1)
[ -n "$lower" ] && [ -n "$load" ] && [ "$lower" -lt "$load" ] &&
	ok "S40network lowers the console before it loads the Wi-Fi module" ||
	bad "S40network: dmesg -n 4 at line '${lower:-none}', insmod at '${load:-none}'"
grep -q 'loglevel=4' "$repo_root/linux/dts/esp32p4-m5stamp-p4.dts" &&
	ok "the Stamp-P4 (no panel) keeps loglevel=4" ||
	bad "Stamp-P4 bootargs changed"

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
