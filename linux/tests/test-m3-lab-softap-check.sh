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
sed -n '/^ap_supported()/,/^}/p;/^ap_live()/,/^}/p' \
	"$script" > "$tmp/funcs.sh"

# Stub iw and dmesg; the check pipes them into real tail/grep.
cat > "$tmp/bin/iw" <<EOF
#!/bin/sh
if [ "\$1" = phy ]; then cat "$tmp/phy"; exit 0; fi
if [ "\$1" = dev ]; then cat "$tmp/info"; exit 0; fi
exit 1
EOF
cat > "$tmp/bin/dmesg" <<EOF
#!/bin/sh
cat "$tmp/dmesg"
EOF
chmod +x "$tmp/bin/iw" "$tmp/bin/dmesg"
PATH="$tmp/bin:$PATH"
export PATH

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

if [ "$fails" -eq 0 ]; then
	echo "PASS: S40network SoftAP liveness checks"
else
	echo "FAIL: $fails checks failed" >&2
	exit 1
fi
