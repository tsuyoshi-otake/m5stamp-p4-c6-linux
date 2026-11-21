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
sed -n '/^ap_supported()/,/^}/p;/^ap_live()/,/^}/p;/^ap_wait_ready()/,/^}/p;/^c6_reset_regs()/,/^}/p;/^c6_reset_pulse()/,/^}/p;/^c6_link_ok()/,/^}/p;/^c6_bind_wait()/,/^}/p;/^c6_reinit()/,/^}/p;/^net_dispatch()/,/^}/p' \
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

# 12. c6_reinit with a link that still answers (the half-up case of #22):
# unbind the mmc host, reset the C6, bind the host again (#41: only the
# host's probe enumerates the card after a reset; a re-probe of the sdio
# function alone times out on every command).  The host is found under
# the driver directory.
C6_OBS="$tmp/obs"
echo "status=ok" > "$C6_OBS"
mkdir -p "$tmp/drv/50083000.mmc"
: > "$tmp/drv/unbind"
: > "$tmp/drv/bind"
rm -f "$tmp/devmem.log"
if C6_MMC_DRV="$tmp/drv" C6_MODEL_FILE="$tmp/model" c6_reinit > "$tmp/reinit.out"; then
	[ "$(cat "$tmp/drv/unbind")" = "50083000.mmc" ] &&
	[ "$(cat "$tmp/drv/bind")" = "50083000.mmc" ] &&
		grep -q '^0x500e000c 32 0x8000' "$tmp/devmem.log" &&
		echo "ok: c6_reinit unbinds the mmc host, resets and binds it again" ||
		{ echo "FAIL: c6_reinit side effects"; fails=$((fails + 1)); }
	grep -q 'rebinding mmc host 50083000.mmc (the C6 link was alive)' "$tmp/reinit.out" &&
		echo "ok: c6_reinit names the host it rebinds and the link state" ||
		{ echo "FAIL: c6_reinit message: $(cat "$tmp/reinit.out")"; fails=$((fails + 1)); }
	if grep -q 'last reset cause' "$tmp/reinit.out"; then
		echo "FAIL: a reset cause was reported for a link the driver itself restarted"
		fails=$((fails + 1))
	else
		echo "ok: no reset cause is reported after a pulse"
	fi
else
	echo "FAIL: c6_reinit returned nonzero"
	fails=$((fails + 1))
fi

# 12a. c6_link_ok reads the first line of the observation node.
echo "status=ok" > "$C6_OBS"
c6_link_ok
expect "c6_link_ok: status=ok answers" "$?"
echo "status=read_fail ret=-110" > "$C6_OBS"
c6_link_ok; rc=$?
[ "$rc" -eq 1 ]
expect "c6_link_ok: read_fail is a dead link" "$?"
echo "status=no_func" > "$C6_OBS"
c6_link_ok; rc=$?
[ "$rc" -eq 1 ]
expect "c6_link_ok: no_func is a dead link" "$?"
rm -f "$C6_OBS"
c6_link_ok; rc=$?
[ "$rc" -eq 2 ]
expect "c6_link_ok: no node means no driver" "$?"

# 12d. A dead link is first rebound WITHOUT the pulse, so that the C6 can
# say why it went down; the last cause in dmesg is the fresh one.
echo "status=read_fail ret=-110" > "$C6_OBS"
: > "$tmp/drv/unbind"
: > "$tmp/drv/bind"
rm -f "$tmp/devmem.log"
cat > "$tmp/dmesg" <<EOF
esp32_sdio: process_fw_data: ESP chipset's last reset cause:
esp32_sdio: print_reset_reason: POWERON_RESET
esp32_sdio: tx_process: Failed to send data: -110 28 28
mmc1: new SDIO card at address 0001
esp32_sdio: process_fw_data: ESP chipset's last reset cause:
esp32_sdio: print_reset_reason: TG0WDT_SYS_RESET
esp32_sdio: check_esp_version: ESP-Hosted Version: NG-1.0.6.0.1
EOF
if C6_MMC_DRV="$tmp/drv" C6_MODEL_FILE="$tmp/model" c6_reinit > "$tmp/reinit.out"; then
	[ "$(cat "$tmp/drv/unbind")" = "50083000.mmc" ] &&
	[ "$(cat "$tmp/drv/bind")" = "50083000.mmc" ] &&
	[ ! -e "$tmp/devmem.log" ] &&
		echo "ok: a dead link is rebound without a reset pulse" ||
		{ echo "FAIL: dead-link rebind side effects"; fails=$((fails + 1)); }
	grep -q 'the C6 link was dead' "$tmp/reinit.out" &&
	grep -q 'its last reset cause: TG0WDT_SYS_RESET$' "$tmp/reinit.out" &&
		echo "ok: the C6's own reset cause is reported" ||
		{ echo "FAIL: reset cause: $(cat "$tmp/reinit.out")"; fails=$((fails + 1)); }
else
	echo "FAIL: c6_reinit on a dead link returned nonzero"
	fails=$((fails + 1))
fi

# 12e. A dead C6 that does not come back by itself gets the pulse after
# all: wlan0 stays away for the ten checks of the plain rebind.
: > "$tmp/drv/unbind"
: > "$tmp/drv/bind"
rm -f "$tmp/devmem.log" "$tmp/ifc"
ifconfig() {
	[ "${2:-}" = down ] && return 0
	n=$(cat "$tmp/ifc" 2>/dev/null || echo 0)
	n=$((n + 1))
	echo "$n" > "$tmp/ifc"
	[ "$n" -gt 10 ]
}
if C6_MMC_DRV="$tmp/drv" C6_MODEL_FILE="$tmp/model" c6_reinit > "$tmp/reinit.out"; then
	grep -q '^0x500e000c 32 0x8000' "$tmp/devmem.log" 2>/dev/null &&
	grep -q 'did not come back by itself; pulsing its reset' "$tmp/reinit.out" &&
	[ "$(cat "$tmp/ifc")" = 11 ] &&
		echo "ok: the pulse follows when the plain rebind brings nothing" ||
		{ echo "FAIL: pulse fallback: $(cat "$tmp/reinit.out")"; fails=$((fails + 1)); }
	if grep -q 'last reset cause' "$tmp/reinit.out"; then
		echo "FAIL: a reset cause was reported after the pulse fallback"
		fails=$((fails + 1))
	else
		echo "ok: the pulse fallback reports no reset cause"
	fi
else
	echo "FAIL: c6_reinit with the pulse fallback returned nonzero"
	fails=$((fails + 1))
fi
ifconfig() { return 1; }
if C6_MMC_DRV="$tmp/drv" C6_MODEL_FILE="$tmp/model" c6_reinit > "$tmp/reinit.out"; then
	echo "FAIL: c6_reinit passed although wlan0 never returned"
	fails=$((fails + 1))
else
	grep -q 'wlan0 did not return after C6 re-init' "$tmp/reinit.out" &&
		echo "ok: c6_reinit fails when wlan0 never returns" ||
		{ echo "FAIL: no-wlan0 message"; fails=$((fails + 1)); }
fi
ifconfig() { return 0; }
echo "status=ok" > "$C6_OBS"

# 12b. The source must not go back to the function-level re-probe.
if sed -n '/^c6_reinit()/,/^}/p' "$script" | grep -q 'drivers_probe'; then
	echo "FAIL: c6_reinit re-probes the sdio function again"
	fails=$((fails + 1))
else
	echo "ok: c6_reinit does not re-probe the sdio function"
fi

# 12c. A host an interrupted attempt left unbound is not under the
# driver any more: nothing to unbind, but it is still bound again.
mkdir -p "$tmp/drv2"
: > "$tmp/drv2/unbind"
: > "$tmp/drv2/bind"
if C6_MMC_DRV="$tmp/drv2" C6_MMC_HOST=50083000.mmc C6_MODEL_FILE="$tmp/model" \
		c6_reinit >/dev/null; then
	[ ! -s "$tmp/drv2/unbind" ] && [ "$(cat "$tmp/drv2/bind")" = "50083000.mmc" ] &&
		echo "ok: an unbound host is only bound" ||
		{ echo "FAIL: unbound host handling"; fails=$((fails + 1)); }
else
	echo "FAIL: c6_reinit with an unbound host returned nonzero"
	fails=$((fails + 1))
fi

# 13. No mmc host driver -> clean skip.
mkdir -p "$tmp/drv-empty"
if C6_MMC_DRV="$tmp/drv-empty" C6_MMC_HOST=50083000.mmc c6_reinit >/dev/null 2>&1; then
	echo "FAIL: c6_reinit passed with no mmc host driver"
	fails=$((fails + 1))
else
	echo "ok: c6_reinit skips when there is no mmc host to rebind"
fi

# 14. The SoftAP retry says what it is about to do before the re-init.
grep -q 'echo "M3-lab: retrying SoftAP once after C6 re-init"' "$script" &&
	echo "ok: start announces the SoftAP retry" ||
	{ echo "FAIL: SoftAP retry message"; fails=$((fails + 1)); }

# 15. apply c6link (asked by bootsyncd's link watch, #41): a dead link is
# re-initialised and the network restarted; a link that answers again, a
# missing driver or a failed re-init restart nothing.
net_note() { echo "$1" >> "$tmp/notes"; }
stop_udhcpd() { :; }
sig_if_running() { :; }
net_reload_seq() { echo reload >> "$tmp/reloads"; }
apply_wpa_hup() { echo hup >> "$tmp/reloads"; }
c6_reinit() { echo "re-init steps"; return "$REINIT_RC"; }
C6_LOG="$tmp/c6.log"
c6link_case() { # name obs-line("" = no node) reinit-rc want-reload want-note
	rm -f "$tmp/notes" "$tmp/reloads" "$C6_LOG" "$C6_OBS"
	[ -n "$2" ] && echo "$2" > "$C6_OBS"
	REINIT_RC=$3
	net_dispatch c6link
	got=0
	[ -f "$tmp/reloads" ] && got=1
	if [ "$got" = "$4" ] && grep -q "$5" "$tmp/notes" 2>/dev/null; then
		echo "ok: apply c6link: $1"
	else
		echo "FAIL: apply c6link: $1 (reload=$got notes: $(cat "$tmp/notes" 2>/dev/null))"
		fails=$((fails + 1))
	fi
}
c6link_case "a dead link is re-initialised and the network restarted" \
	"status=read_fail ret=-110" 0 1 "C6 link is dead; re-initialising"
grep -q 're-init steps' "$C6_LOG" 2>/dev/null &&
	echo "ok: apply c6link: the re-init steps reach the console" ||
	{ echo "FAIL: apply c6link: re-init output lost"; fails=$((fails + 1)); }
c6link_case "a failed re-init restarts nothing" \
	"status=read_fail ret=-110" 1 0 "C6 re-init failed"
c6link_case "a link that answers again is left alone" \
	"status=ok" 0 0 "answers again; no re-init"
c6link_case "no driver, nothing to do" \
	"" 0 0 "no C6 driver loaded"

if [ "$fails" -eq 0 ]; then
	echo "PASS: S40network SoftAP liveness checks"
else
	echo "FAIL: $fails checks failed" >&2
	exit 1
fi
