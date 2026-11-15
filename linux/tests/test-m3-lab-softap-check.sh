#!/bin/sh
# S40network SoftAP checks (#22): ap_supported reads the driver's mode list,
# ap_live wants an ssid in `iw dev wlan0 info` and a clean dmesg delta.
set -u

repo_root=$(cd -- "$(dirname -- "$0")/../.." && pwd)
script="$repo_root/linux/m3-lab/rootfs-overlay/etc/init.d/S40network"
tmp=${TMPDIR:-/tmp}/softap-check-test.$$
trap 'rm -rf -- "$tmp"' EXIT HUP INT TERM
mkdir -p "$tmp/bin"

# Source just the check functions; globals they use are set below.
sed -n '/^ap_supported()/,/^}/p;/^ap_live()/,/^}/p;/^ap_wait_ready()/,/^}/p;/^c6_reset_regs()/,/^}/p;/^c6_reset_pulse()/,/^}/p;/^c6_reinit()/,/^}/p' \
	"$script" > "$tmp/funcs.sh"

# Stub iw and dmesg; the check pipes them into real tail/grep.  iw dev
# counts its calls: below $tmp/info-delay it prints $tmp/info-early
# (used for "the ssid shows up late"); from then on $tmp/info.
cat > "$tmp/bin/iw" <<EOF
#!/bin/sh
if [ "\$1" = phy ]; then cat "$tmp/phy"; exit 0; fi
if [ "\$1" = dev ]; then
	n=\$(cat "$tmp/iwcalls" 2>/dev/null || echo 0)
	n=\$((n + 1))
	echo "\$n" > "$tmp/iwcalls"
	if [ -f "$tmp/info-delay" ] && [ "\$n" -lt "\$(cat "$tmp/info-delay")" ]; then
		cat "$tmp/info-early"
	else
		cat "$tmp/info"
	fi
	exit 0
fi
exit 1
EOF
cat > "$tmp/bin/dmesg" <<EOF
#!/bin/sh
cat "$tmp/dmesg"
EOF
cat > "$tmp/bin/devmem" <<EOF
#!/bin/sh
echo "\$@" >> "$tmp/devmem.log"
EOF
chmod +x "$tmp/bin/iw" "$tmp/bin/dmesg" "$tmp/bin/devmem"
PATH="$tmp/bin:$PATH"
export PATH

# Shell stubs for the re-init path: no sleeping, wlan0 stays up, wpa
# stop is a no-op, and devmem is logged by the file stub above.
net_tick() { :; }
sleep() { :; }
stop_wpa() { :; }
ifconfig() { return 0; }

AP_WHY=""
. "$tmp/funcs.sh"

fails=0
expect() { # name rc
	if [ "$2" -eq 0 ]; then
		echo "ok: $1"
	else
		echo "FAIL: $1"
		fails=$((fails + 1))
	fi
}

# iw phy modes list with AP (as on a driver built with CONFIG_AP_MODE).
cat > "$tmp/phy" <<EOF
Wiphy phy0
	Supported interface modes:
		 * IBSS
		 * managed
		 * AP
		 * P2P-client
EOF

# 1. AP in the supported modes list -> supported.
ap_supported
expect "iw phy with AP is supported" "$?"

# 2. Station-only wiphy -> not supported.
cat > "$tmp/phy" <<EOF
Wiphy phy0
	Supported interface modes:
		 * IBSS
		 * managed
		 * P2P-client
EOF
if ap_supported; then
	echo "FAIL: station-only wiphy passed ap_supported"
	fails=$((fails + 1))
else
	echo "ok: station-only wiphy is rejected"
fi

# Live AP info as `iw dev wlan0 info` prints it once beacons run.
cat > "$tmp/info" <<EOF
Interface wlan0
	ifindex 5
	wdev 0x2
	addr 02:00:00:00:00:01
	ssid m5-E528
	type AP
	wiphy 0
	channel 6 (2437 MHz), width: 20 MHz, center1: 2437 MHz
	txpower 20.00 dBm
EOF

cat > "$tmp/dmesg" <<EOF
[   12.3] esp32_sdio: esp_probe: network adapter init
[   13.1] IPv6: ADDRCONF(NETDEV_CHANGE): wlan0: link becomes ready
EOF

# 3. type AP + ssid + clean dmesg delta -> live.
ap_live 3
expect "live AP (ssid, clean link)" "$?"

# 4. type AP but no ssid -> not on air.
cat > "$tmp/info" <<EOF
Interface wlan0
	ifindex 5
	wdev 0x2
	addr 02:00:00:00:00:01
	type AP
	wiphy 0
EOF
if ap_live 3; then
	echo "FAIL: AP without ssid passed"
	fails=$((fails + 1))
else
	case "$AP_WHY" in
	*ssid*) echo "ok: AP without ssid rejected ($AP_WHY)" ;;
	*) echo "FAIL: wrong reason: $AP_WHY"; fails=$((fails + 1)) ;;
	esac
fi

# 5. Interface still in managed mode -> not in AP mode, type is reported.
cat > "$tmp/info" <<EOF
Interface wlan0
	ifindex 5
	wdev 0x2
	addr 02:00:00:00:00:01
	type managed
	wiphy 0
EOF
if ap_live 3; then
	echo "FAIL: managed iface passed"
	fails=$((fails + 1))
else
	case "$AP_WHY" in
	*not\ in\ AP\ mode*managed*) echo "ok: managed iface rejected ($AP_WHY)" ;;
	*) echo "FAIL: wrong reason: $AP_WHY"; fails=$((fails + 1)) ;;
	esac
fi

# 6. SDIO errors that arrived only after the attempt began -> not live.
cat > "$tmp/info" <<EOF
Interface wlan0
	ifindex 5
	ssid m5-E528
	type AP
EOF
cat > "$tmp/dmesg" <<EOF
[    9.0] esp32_sdio: esp_probe: network adapter init
[   12.3] M3-lab: SoftAP channel 11 (2462 MHz)
[   12.4] esp32_sdio: tx_process: Failed to send data: -110
[   12.5] esp_handle_isr: CMD53 read/write error at 413 / 507
EOF
if ap_live 3; then
	echo "FAIL: AP with fresh SDIO errors passed"
	fails=$((fails + 1))
else
	echo "ok: fresh SDIO errors rejected ($AP_WHY)"
fi

# 7. The same errors present BEFORE the marker do not fail the attempt
# (only the delta since the attempt began is judged).
cat > "$tmp/dmesg" <<EOF
[    9.0] esp32_sdio: tx_process: Failed to send data: -110
[    9.1] esp_handle_isr: CMD53 read/write error at 413 / 507
[   12.3] esp32_sdio: esp_probe: network adapter init
EOF
ap_live 4
expect "old SDIO errors before marker are ignored" "$?"

# 8. The ssid shows up only on the 4th poll -> ap_wait_ready still
# succeeds (review: up to ~10 s at 1 s cadence; net_tick is a no-op).
rm -f "$tmp/iwcalls"
echo 4 > "$tmp/info-delay"
cat > "$tmp/info-early" <<EOF
Interface wlan0
	addr 02:00:00:00:00:01
	type AP
EOF
cat > "$tmp/info" <<EOF
Interface wlan0
	ssid m5-E528
	type AP
EOF
cat > "$tmp/dmesg" <<EOF
[   12.3] esp32_sdio: esp_probe: network adapter init
EOF
if ap_wait_ready 1; then
	ncalls=$(cat "$tmp/iwcalls")
	[ "$ncalls" -ge 4 ] &&
		echo "ok: late ssid (4th poll) is accepted ($ncalls iw calls)" ||
		{ echo "FAIL: late ssid accepted after only $ncalls calls"; fails=$((fails + 1)); }
else
	echo "FAIL: late ssid rejected ($AP_WHY)"
	fails=$((fails + 1))
fi
rm -f "$tmp/info-delay" "$tmp/info-early" "$tmp/iwcalls"

# 9. Fresh SDIO errors abort the wait on the first poll, not after 10 s.
rm -f "$tmp/iwcalls"
cat > "$tmp/dmesg" <<EOF
[    9.0] esp32_sdio: esp_probe: network adapter init
[   12.4] esp32_sdio: tx_process: Failed to send data: -110
EOF
if ap_wait_ready 2; then
	echo "FAIL: dead link waited the full window"
	fails=$((fails + 1))
else
	ncalls=$(cat "$tmp/iwcalls")
	[ "$ncalls" -eq 1 ] &&
		echo "ok: SDIO errors abort the wait early" ||
		{ echo "FAIL: SDIO error path polled $ncalls times"; fails=$((fails + 1)); }
fi
rm -f "$tmp/iwcalls"

# 10. c6_reset_regs picks the board's GPIO.
case "$(c6_reset_regs 'M5Stack Tab5')" in
"0x500e0024 0x500e000c 0x500e0008 0x8000") echo "ok: tab5 reset regs" ;;
*) echo "FAIL: tab5 reset regs"; fails=$((fails + 1)) ;;
esac
case "$(c6_reset_regs 'Stamp-P4 Rev0.15 (M1)')" in
"0x500e0030 0x500e0018 0x500e0014 0x400") echo "ok: stamp reset regs" ;;
*) echo "FAIL: stamp reset regs"; fails=$((fails + 1)) ;;
esac
if c6_reset_regs 'something else' >/dev/null 2>&1; then
	echo "FAIL: unknown board got reset regs"
	fails=$((fails + 1))
else
	echo "ok: unknown board has no reset regs"
fi

# 11. c6_reset_pulse pokes enable -> assert -> release on the tab5 GPIO.
printf 'M5Stack Tab5\0' > "$tmp/model"
rm -f "$tmp/devmem.log"
C6_MODEL_FILE="$tmp/model" c6_reset_pulse &&
	echo "ok: c6_reset_pulse runs on tab5 model" ||
	{ echo "FAIL: c6_reset_pulse tab5"; fails=$((fails + 1)); }
[ "$(sed -n '2p;3p' "$tmp/devmem.log" | tr '\n' ';')" = "0x500e000c 32 0x8000;0x500e0008 32 0x8000;" ] &&
	echo "ok: pulse drives GPIO15 low then high" ||
	{ echo "FAIL: pulse sequence: $(cat "$tmp/devmem.log")"; fails=$((fails + 1)); }

# 12. c6_reinit: unbind the sdio function, reset, drivers_probe.
# C6_SDIO_DEV injects the function path (Windows filesystems cannot
# create a directory with ':' in its name like real sysfs has).
mkdir -p "$tmp/sys/devices/mmc0-0001-1/driver"
: > "$tmp/sys/devices/mmc0-0001-1/driver/unbind"
: > "$tmp/sys/drivers_probe"
rm -f "$tmp/devmem.log"
if C6_SDIO_BUS="$tmp/sys" C6_SDIO_DEV="$tmp/sys/devices/mmc0-0001-1" \
		C6_MODEL_FILE="$tmp/model" c6_reinit; then
	[ "$(cat "$tmp/sys/devices/mmc0-0001-1/driver/unbind")" = "mmc0-0001-1" ] &&
	[ "$(cat "$tmp/sys/drivers_probe")" = "mmc0-0001-1" ] &&
		grep -q '^0x500e000c 32 0x8000' "$tmp/devmem.log" &&
		echo "ok: c6_reinit unbinds, resets and re-probes" ||
		{ echo "FAIL: c6_reinit side effects"; fails=$((fails + 1)); }
else
	echo "FAIL: c6_reinit returned nonzero"
	fails=$((fails + 1))
fi

# 13. No sdio function device -> clean skip.
mkdir -p "$tmp/sys-empty/devices"
if C6_SDIO_BUS="$tmp/sys-empty" c6_reinit 2>/dev/null; then
	echo "FAIL: c6_reinit passed with no sdio function"
	fails=$((fails + 1))
else
	echo "ok: c6_reinit skips when no sdio function is bound"
fi

if [ "$fails" -eq 0 ]; then
	echo "PASS: S40network SoftAP liveness checks"
else
	echo "FAIL: $fails checks failed" >&2
	exit 1
fi
