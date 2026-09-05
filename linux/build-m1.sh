#!/usr/bin/env bash
set -euo pipefail

usage() {
	cat >&2 <<'EOF'
Usage: build-m1.sh <external-source-root|--vendor> <build-output> [--print-only] [--profile m1|m2|m3|m3-lab]

The source root is either the directory populated by tools/fetch-sources.sh or
the checked-in vendor submodules after `tools/fetch-sources.sh --vendor`.
Buildroot output and temporary patch staging remain outside the repository.
This script builds artifacts only; it never invokes esptool or writes flash.
C68-CLEAN-RELEASE is not this entry point: use build-c68.sh so boot-shim nm
addresses are rendered before 0038/0039 are staged.
Set EASYSTICK_TSENS_LINUX=1 to include the experimental ESP32-P4 LP-TSENS
driver, its REGI2C DAC readback gate, and the thermal sysfs endpoint.  This
requires the C68 profile and is separate from EASYSTICK_TSENS_ORACLE=1.
Set EASYSTICK_ESPHOSTED_DIAGNOSTICS=1 for full ESP-Hosted console breadcrumbs.
The default M3-lab image keeps those patches but applies 0024 so esp_info() in
esp_sdio.c stays at debug level and does not starve ARP/SSH on NOMMU.
Set EASYSTICK_SDIO_DEFAULT_MHZ to one of 1, 2, 5, or 10 to build a distinct
SDIO clock arm.  The value is compiled into esp32_sdio.ko and attested at boot.
Set EASYSTICK_WDT_TEST_TIMEOUT_S to 1..3600 for a bounded watchdog positive
control; the value is appended to the staged Linux bootargs only.
Set EASYSTICK_WDT_TEST_NO_FEED=1 together with the timeout to disable the
watchdog feed in that test image; it is rejected for non-test builds.
Set EASYSTICK_WDT_INJECT=0 to skip 0059. Default (BB builds) adds
`esp32p4_wdt.inject=1` (IRQ-on spin) and `=2` (local_irq_disable then spin).
Set EASYSTICK_BUILDROOT_JLEVEL=1 to serialize nested Buildroot package makes.
This is useful for BusyBox versions whose generated applet tables are not safe
when install-noclobber is invoked in parallel.
Set EASYSTICK_CMD53_BB_ALLOW_PIO=1 only for the explicit EASYSTICK_LINUX_ONLY=1
retention-BB PIO A/B; the normal retention-BB contract remains DMA-only.
Set EASYSTICK_STACKTRACE_DIAGNOSTICS=1 for the P4-only diagnostic image.  This
requires the retention-BB build and enables bounded WDT stack output,
KALLSYMS/DWARF data, and the CMD52 token-read boundary trace.
Set EASYSTICK_ESPHOSTED_CMD52_TRACE=0 with that diagnostic image to retain the
stacktrace/WDT build while omitting only the CMD52 UART observer.
Set EASYSTICK_ESPHOSTED_CMD52_MARKER=1 with CMD52_TRACE=0 to retain the
six-word CMD52 positive-control marker in the P4 retention area.  It writes
MARKER_MAGIC/ARMED before SDIO registration, then records TOKEN_ENTER,
AFTER_46, BEFORE_47, and AFTER_47 in separate words.
When the marker is enabled, the module also exposes the root-only
cmd52_marker_shot parameter; writing 1 to it clears the four stage words and
re-arms MAGIC/ARMED after normal Wi-Fi startup for one clean shot.
EOF
}

if [[ $# -lt 2 || $# -gt 5 ]]; then
	usage
	exit 2
fi

source_arg=$1
output=$(mkdir -p -- "$2" && cd -- "$2" && pwd)
profile=m1
print_only=false
shift 2
while [[ $# -gt 0 ]]; do
	case "$1" in
		--print-only) print_only=true; shift ;;
		--profile)
			[[ $# -ge 2 ]] || { usage; exit 2; }
			profile=$2
			shift 2
			;;
		*) usage; exit 2 ;;
	esac
done
network_profile=false
ssh_profile=false
lab_profile=false
case "$profile" in
	m1) ;;
	m2) network_profile=true ;;
	m3) network_profile=true; ssh_profile=true ;;
	m3-lab) network_profile=true; ssh_profile=true; lab_profile=true ;;
	*) echo "unsupported profile: $profile" >&2; exit 2 ;;
esac
sdio_default_mhz="${EASYSTICK_SDIO_DEFAULT_MHZ:-5}"
case "$sdio_default_mhz" in
	1|2|5|10) ;;
	*) echo "EASYSTICK_SDIO_DEFAULT_MHZ must be 1, 2, 5, or 10" >&2; exit 2 ;;
esac
wdt_test_timeout_s="${EASYSTICK_WDT_TEST_TIMEOUT_S:-0}"
case "$wdt_test_timeout_s" in
	0) ;;
	''|*[!0-9]*) echo "EASYSTICK_WDT_TEST_TIMEOUT_S must be 0 or an integer from 1 to 3600" >&2; exit 2 ;;
	*) (( wdt_test_timeout_s >= 1 && wdt_test_timeout_s <= 3600 )) || {
		echo "EASYSTICK_WDT_TEST_TIMEOUT_S must be 0 or an integer from 1 to 3600" >&2
		exit 2
	} ;;
esac
wdt_test_no_feed="${EASYSTICK_WDT_TEST_NO_FEED:-0}"
case "$wdt_test_no_feed" in
	0|1) ;;
	*) echo "EASYSTICK_WDT_TEST_NO_FEED must be 0 or 1" >&2; exit 2 ;;
esac
if [[ "$wdt_test_no_feed" != "0" && "$wdt_test_timeout_s" == "0" ]]; then
	echo "EASYSTICK_WDT_TEST_NO_FEED requires EASYSTICK_WDT_TEST_TIMEOUT_S" >&2
	exit 2
fi
wdt_inject="${EASYSTICK_WDT_INJECT:-1}"
case "$wdt_inject" in
	0|1) ;;
	*) echo "EASYSTICK_WDT_INJECT must be 0 or 1" >&2; exit 2 ;;
esac
stacktrace_diagnostics="${EASYSTICK_STACKTRACE_DIAGNOSTICS:-0}"
case "$stacktrace_diagnostics" in
	0|1) ;;
	*) echo "EASYSTICK_STACKTRACE_DIAGNOSTICS must be 0 or 1" >&2; exit 2 ;;
esac
export EASYSTICK_STACKTRACE_DIAGNOSTICS="$stacktrace_diagnostics"
if [[ "$stacktrace_diagnostics" == "1" &&
      "${EASYSTICK_CMD53_RETENTION_BB:-0}" != "1" ]]; then
	echo "EASYSTICK_STACKTRACE_DIAGNOSTICS requires the retention-BB build" >&2
	exit 2
fi
cmd53_bb_allow_pio="${EASYSTICK_CMD53_BB_ALLOW_PIO:-0}"
case "$cmd53_bb_allow_pio" in
	0|1) ;;
	*) echo "EASYSTICK_CMD53_BB_ALLOW_PIO must be 0 or 1" >&2; exit 2 ;;
esac
if [[ "$cmd53_bb_allow_pio" == "1" &&
      ( "${EASYSTICK_LINUX_ONLY:-0}" != "1" ||
        "${EASYSTICK_CMD53_RETENTION_BB:-0}" != "1" ) ]]; then
	echo "EASYSTICK_CMD53_BB_ALLOW_PIO requires EASYSTICK_LINUX_ONLY=1 and retention BB" >&2
	exit 2
fi
if [[ "$stacktrace_diagnostics" == "1" && "$network_profile" != true ]]; then
	echo "EASYSTICK_STACKTRACE_DIAGNOSTICS requires a network profile" >&2
	exit 2
fi
cmd52_trace="${EASYSTICK_ESPHOSTED_CMD52_TRACE:-$stacktrace_diagnostics}"
case "$cmd52_trace" in
	0|1) ;;
	*) echo "EASYSTICK_ESPHOSTED_CMD52_TRACE must be 0 or 1" >&2; exit 2 ;;
esac
if [[ "$cmd52_trace" == "1" && "$stacktrace_diagnostics" != "1" ]]; then
	echo "EASYSTICK_ESPHOSTED_CMD52_TRACE requires stacktrace diagnostics" >&2
	exit 2
fi
export EASYSTICK_ESPHOSTED_CMD52_TRACE="$cmd52_trace"
cmd52_marker="${EASYSTICK_ESPHOSTED_CMD52_MARKER:-0}"
case "$cmd52_marker" in
	0|1) ;;
	*) echo "EASYSTICK_ESPHOSTED_CMD52_MARKER must be 0 or 1" >&2; exit 2 ;;
esac
if [[ "$cmd52_marker" == "1" &&
      "${EASYSTICK_CMD53_RETENTION_BB:-0}" != "1" ]]; then
	echo "EASYSTICK_ESPHOSTED_CMD52_MARKER requires the retention-BB build" >&2
	exit 2
fi
if [[ "$cmd52_marker" == "1" && "$stacktrace_diagnostics" != "1" ]]; then
	echo "EASYSTICK_ESPHOSTED_CMD52_MARKER requires stacktrace diagnostics" >&2
	exit 2
fi
if [[ "$cmd52_marker" == "1" && "$cmd52_trace" == "1" ]]; then
	echo "EASYSTICK_ESPHOSTED_CMD52_MARKER and CMD52_TRACE are mutually exclusive" >&2
	exit 2
fi
export EASYSTICK_ESPHOSTED_CMD52_MARKER="$cmd52_marker"
if [[ "$wdt_test_timeout_s" != "0" &&
      "${EASYSTICK_CMD53_RETENTION_BB:-0}" != "1" ]]; then
	echo "EASYSTICK_WDT_TEST_TIMEOUT_S requires the retention-BB build" >&2
	exit 2
fi
if ! $network_profile && [[ "$sdio_default_mhz" != "5" ]]; then
	echo "EASYSTICK_SDIO_DEFAULT_MHZ is only valid for a network profile" >&2
	exit 2
fi

script_dir=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)
firmware_root=$(cd -- "${script_dir}/.." && pwd)
external_source_root="${script_dir}/buildroot-external"
lock_file="${firmware_root}/versions.lock.json"
if [[ "$source_arg" == "--vendor" ]]; then
	source_root="${firmware_root}/vendor"
	why_root="${source_root}/why2025-linux"
	linux_root="${source_root}/linux"
	buildroot_root="${source_root}/buildroot"
	hosted_root="${source_root}/esp-hosted/esp_hosted_ng/host"
else
	source_root=$(cd -- "$source_arg" && pwd)
	why_root="${source_root}/why2025_linux_reference"
	linux_root="${source_root}/linux_base"
	buildroot_root="${source_root}/buildroot"
	hosted_root="${source_root}/esp_hosted_ng/esp_hosted_ng/host"
fi
br_output="${output}/buildroot"
profile_stamp="${br_output}/.easystick-build-profile"
patch_stage="${output}/kernel-patches"
dts_stage="${output}/dts"
kernel_config_fragment="${output}/linux.config"
external_root="${output}/buildroot-external"
global_patch_stage="${output}/buildroot-global-patches"
m3_stage="${output}/m3-profile"
lab_stage="${output}/m3-lab-profile"
lab_provision_stage="${output}/m3-lab-provision"
c68_profile=false
if [[ "${EASYSTICK_C68_CLEAN_RELEASE:-0}" == "1" ]]; then
	c68_profile=true
fi
if [[ "${EASYSTICK_TSENS_LINUX:-0}" == "1" && "$c68_profile" != true ]]; then
	echo "EASYSTICK_TSENS_LINUX requires the C68 profile" >&2
	exit 2
fi
c68_stress_profile=false
c68_top_profile=false
if [[ "${EASYSTICK_C68_TOP:-0}" == "1" ||
      "${EASYSTICK_C68_L3_SMOKE:-0}" == "1" ||
      "${EASYSTICK_C68_STRESS:-0}" == "1" ]]; then
	c68_top_profile=true
fi
if [[ "${EASYSTICK_C68_STRESS:-0}" == "1" ]]; then
	c68_stress_profile=true
fi
if $c68_top_profile; then
	c68_profile_stage="${output}/c68-profile"
	rm -rf -- "$c68_profile_stage"
	mkdir -p -- "$c68_profile_stage"
	cp -- "${script_dir}/c68/busybox.fragment" \
		"${c68_profile_stage}/busybox.fragment"
fi

# Buildroot asks Git whether BR2_EXTERNAL is dirty while resolving the
# external tree.  Stage the small board definition outside the repository so
# this check is deterministic and does not walk the entire workspace (or its
# submodules) on every build.
rm -rf -- "$external_root"
mkdir -p -- "$external_root"
cp -a -- "${external_source_root}/." "$external_root/"
if $network_profile; then
	mkdir -p -- "${external_root}/board/easystick-stamp-p4/rootfs-overlay/etc/easystick"
	printf '%s\n' "$sdio_default_mhz" \
		>"${external_root}/board/easystick-stamp-p4/rootfs-overlay/etc/easystick/sdio-clock-mhz"
	python3 - "${external_root}/package/esp-hosted-ng/0002-easystick-stamp-p4-sdio-defaults.patch" "$sdio_default_mhz" <<'PY'
from pathlib import Path
import re
import sys

path = Path(sys.argv[1])
mhz = sys.argv[2]
text = path.read_text(encoding="utf-8")
pattern = re.compile(r"^\+static u32 clockspeed = [0-9]+;$", re.MULTILINE)
matches = pattern.findall(text)
if len(matches) != 1:
    raise SystemExit(
        f"SDIO_CLOCK_GATE_FAIL: expected one clockspeed patch line, found {len(matches)}"
    )
text = pattern.sub(f"+static u32 clockspeed = {mhz};", text, count=1)
path.write_text(text, encoding="utf-8", newline="\n")
PY
fi
if ! $network_profile; then
	# The shared board overlay contains the network bring-up scripts used by
	# M2/M3.  They are not harmless in M1: S40network starts a background
	# wlan0/DHCP wait loop even when the driver is absent, consuming process
	# slots and obscuring a bounded SMP test.  M1 also has no writable /var,
	# so seedrng's early write attempt is not useful there and can expose a
	# read-only-NOMMU startup fault.  Remove these inputs from the disposable
	# non-network profile.  Also remove them in post-build: an old shared
	# Buildroot target directory can retain files after its overlay input is
	# gone.
	rm -f -- \
		"${external_root}/board/easystick-stamp-p4/rootfs-overlay/etc/init.d/S40network" \
		"${external_root}/board/easystick-stamp-p4/rootfs-overlay/etc/init.d/S41timesync" \
		"${external_root}/board/easystick-stamp-p4/rootfs-overlay/usr/sbin/easystick-sdio-diag"
	cat >>"${external_root}/board/easystick-stamp-p4/post-build.sh" <<'EOF'

# Non-network profile boundary: do not let a reused Buildroot target retain
# network-only files from an earlier M2/M3 build.
rm -f -- \
	"${TARGET_DIR}/etc/init.d/S40network" \
	"${TARGET_DIR}/etc/init.d/S41timesync" \
	"${TARGET_DIR}/etc/init.d/S01seedrng" \
	"${TARGET_DIR}/usr/sbin/easystick-sdio-diag"
EOF
fi

# Experimental A/B control: omit only the bounded probe-time BOOT_POLL patch
# from the disposable BR2_EXTERNAL copy.  The checked-in series stays intact,
# so 0005's delayed manual esp_process_new_packet_intr() receive path and all
# later patches continue to apply unchanged.
if [[ "${EASYSTICK_ESPHOSTED_DISABLE_0010:-0}" == "1" ]]; then
	rm -- "${external_root}/package/esp-hosted-ng/0010-easystick-sdio-boot-packet-len-poll.patch"
fi
# Quiet default: keep the functional diagnostic patches (later patches in the
# series depend on their context) but demote console spam via 0024 (esp_info)
# and 0025 (remove BOOT_CMD53/RXTRACE console dumps; loglevel=8 still prints
# KERN_DEBUG) so formatting does not starve ARP/Dropbear on this NOMMU target.  Opt in to full console breadcrumbs with
# EASYSTICK_ESPHOSTED_DIAGNOSTICS=1 (drops 0024+0025 only).
if [[ "${EASYSTICK_ESPHOSTED_DIAGNOSTICS:-0}" == "1" ]]; then
	rm -f -- \
		"${external_root}/package/esp-hosted-ng/0024-easystick-quiet-sdio-diagnostics.patch" \
		"${external_root}/package/esp-hosted-ng/0025-easystick-quiet-sdio-hexdumps.patch"
fi
# Observe-only P4 host TX stage ledger (ES_TX … CMD53). Opt-in only: the
# markers use KERN_EMERG and sit on the SSH TX hot path, so keep them out of
# the default quiet acceptance image.
if [[ "${EASYSTICK_ESPHOSTED_TX_LEDGER:-0}" != "1" ]]; then
	rm -f -- \
		"${external_root}/package/esp-hosted-ng/0023-easystick-sdio-tx-stage-ledger.patch"
fi
# P4-only stacktrace/CMD52 boundary observer.  It is deliberately absent from
# every other image because KERN_EMERG per-byte markers are instrumentation,
# not acceptance-path behavior.  The CMD52 trace can also be omitted from a
# stacktrace image for a no-console-instrumentation control.
if [[ "$cmd52_trace" != "1" ]]; then
	rm -f -- \
		"${external_root}/package/esp-hosted-ng/0029-easystick-sdio-cmd52-boundaries.patch"
fi
if [[ "$cmd52_trace" != "1" &&
      -e "${external_root}/package/esp-hosted-ng/0029-easystick-sdio-cmd52-boundaries.patch" ]]; then
	echo "CMD52_TRACE_GATE_FAIL: observer patch was not removed" >&2
	exit 1
fi
# P4-only CMD52 positive-control marker.  The patch is rendered from the same
# boot-shim PA as 0052; never stage its BBDEAD template directly.  The optional
# shot-gate patch is staged only with the marker so a non-marker image cannot
# expose a control that has no retention contract.
rm -f -- \
	"${external_root}/package/esp-hosted-ng/0030-easystick-sdio-cmd52-retention-marker.patch" \
	"${external_root}/package/esp-hosted-ng/0031-easystick-sdio-cmd52-shot-gate.patch"
if [[ "$cmd52_marker" == "1" ]]; then
	marker_patch="${EASYSTICK_CMD53_BB_ESPHOSTED_MARKER_PATCH:-}"
	if [[ -z "$marker_patch" && -n "${EASYSTICK_KERNEL_PATCH_OVERRIDE_DIR:-}" ]]; then
		marker_patch="${EASYSTICK_KERNEL_PATCH_OVERRIDE_DIR}/0030-easystick-sdio-cmd52-retention-marker.patch"
	fi
	[[ -n "$marker_patch" && -f "$marker_patch" ]] || {
		echo "CMD52_MARKER_GATE_FAIL: missing rendered esp-hosted marker patch" >&2
		exit 1
	}
	if grep -q BBDEAD "$marker_patch"; then
		echo "CMD52_MARKER_GATE_FAIL: placeholder remains in marker patch" >&2
		exit 1
	fi
	cp -- "$marker_patch" \
		"${external_root}/package/esp-hosted-ng/0030-easystick-sdio-cmd52-retention-marker.patch"
	shot_gate_patch="${EASYSTICK_KERNEL_PATCH_OVERRIDE_DIR:-}/0031-easystick-sdio-cmd52-shot-gate.patch"
	if [[ ! -f "$shot_gate_patch" ]]; then
		shot_gate_patch="${external_source_root}/package/esp-hosted-ng/0031-easystick-sdio-cmd52-shot-gate.patch"
	fi
	[[ -f "$shot_gate_patch" ]] || {
		echo "CMD52_MARKER_GATE_FAIL: missing rendered shot-gate patch" >&2
		exit 1
	}
	cp -- "$shot_gate_patch" \
		"${external_root}/package/esp-hosted-ng/0031-easystick-sdio-cmd52-shot-gate.patch"
fi
# Experiment 0053 BB6: install nm-rendered esp-hosted crumb only for retention BB.
rm -f -- "${external_root}/package/esp-hosted-ng/0026-easystick-cmd53-retention-bb6.patch"
if [[ "${EASYSTICK_CMD53_RETENTION_BB:-0}" == "1" ]]; then
	esp_bb6="${EASYSTICK_CMD53_BB_ESPHOSTED_PATCH:-}"
	if [[ -z "$esp_bb6" && -n "${EASYSTICK_KERNEL_PATCH_OVERRIDE_DIR:-}" ]]; then
		esp_bb6="${EASYSTICK_KERNEL_PATCH_OVERRIDE_DIR}/0026-easystick-cmd53-retention-bb6.patch"
	fi
	[[ -n "$esp_bb6" && -f "$esp_bb6" ]] || {
		echo "CMD53_BB_GATE_FAIL: missing rendered esp-hosted BB6 patch" >&2
		exit 1
	}
	if grep -q BBDEAD "$esp_bb6"; then
		echo "CMD53_BB_GATE_FAIL: placeholder remains in esp-hosted BB6 patch" >&2
		exit 1
	fi
	cp -- "$esp_bb6" "${external_root}/package/esp-hosted-ng/0026-easystick-cmd53-retention-bb6.patch"
fi

buildroot_for_make="$buildroot_root"
if $network_profile; then
	# M2 needs one Buildroot-tree patch to make wpa_supplicant selectable on
	# NOMMU.  Keep the locked submodule pristine: archive its exact commit into
	# the external output directory and patch that disposable copy.
	buildroot_for_make="${output}/buildroot-source"
	rm -rf -- "$buildroot_for_make"
	mkdir -p -- "$buildroot_for_make"
	git -C "$buildroot_root" archive --format=tar HEAD | tar -x -C "$buildroot_for_make"
	patch -p1 -d "$buildroot_for_make" \
		<"${script_dir}/m2/buildroot-patches/package-wpa-nommu.patch"
	rm -rf -- "$global_patch_stage"
	mkdir -p -- "${global_patch_stage}/wpa_supplicant"
	cp -- "${script_dir}/m2/buildroot-patches/wpa-os-unix-vfork.patch" \
		"${global_patch_stage}/wpa_supplicant/0001-os-unix-vfork-for-nommu.patch"
	# M3-lab only: Dropbear SSH stage ledger (kmsg/ttyGS1 markers) and pacing patch.
	if $lab_profile; then
		mkdir -p -- "${global_patch_stage}/dropbear"
		if [[ "${EASYSTICK_SSH_LEDGER:-0}" == "1" ]]; then
			cp -- "${script_dir}/m3-lab/dropbear-patches/"*.patch \
				"${global_patch_stage}/dropbear/"
		else
			if [[ -f "${script_dir}/m3-lab/dropbear-patches/0005-easystick-ssh-close-pacing.patch" ]]; then
				cp -- "${script_dir}/m3-lab/dropbear-patches/0005-easystick-ssh-close-pacing.patch" \
					"${global_patch_stage}/dropbear/"
			fi
		fi
		# BusyBox post-exec markers are surgical injects (see
		# m3-lab/busybox-patches/README.md); no BR2 patch series yet because
		# the tree is hush-based and the inject is source-local.
	fi
fi

if $ssh_profile; then
	# Stage the selected SSH profile outside BR2_EXTERNAL.  The repository is
	# often on a Windows filesystem, while Buildroot runs in a Linux container;
	# absolute generated paths avoid a second checkout and keep the source tree
	# read-only during the build.
	rm -rf -- "$m3_stage" "$lab_stage" "$lab_provision_stage"
	cp -a -- "${script_dir}/m3/." "$m3_stage/"
	if $lab_profile; then
		cp -a -- "${script_dir}/m3-lab/." "$lab_stage/"
		mkdir -p -- "${lab_provision_stage}/etc/easystick"
		python3 - "${lab_provision_stage}/etc/easystick/wpa_supplicant.conf" <<'PY'
import json
import os
import sys
from pathlib import Path

path = Path(sys.argv[1])
mode = os.environ.get("EASYSTICK_WIFI_MODE", "ap")
ssid = os.environ.get("EASYSTICK_WIFI_SSID", "m5")
psk = os.environ.get("EASYSTICK_WIFI_PSK", "m5stamp-p4-c6")

if mode == "ap":
    conf = (
        "update_config=0\n"
        "ap_scan=1\n\n"
        "network={\n"
        f"\tssid={json.dumps(ssid, ensure_ascii=False)}\n"
        "\tmode=2\n"
        "\tfrequency=2437\n"
        "\tkey_mgmt=WPA-PSK\n"
        "\tproto=RSN\n"
        "\tpairwise=CCMP\n"
        "\tgroup=CCMP\n"
        f"\tpsk={json.dumps(psk, ensure_ascii=False)}\n"
        "}\n"
    )
else:
    conf = (
        "update_config=0\n"
        "country=JP\n"
        "network={\n"
        f"\tssid={json.dumps(ssid, ensure_ascii=False)}\n"
        f"\tpsk={json.dumps(psk, ensure_ascii=False)}\n"
        "}\n"
    )
path.write_text(conf, encoding="utf-8")
path.chmod(0o600)
PY
	fi
fi

for command in git python3 make patch tar file cpio rsync bc; do
	command -v "$command" >/dev/null || { echo "missing host command: $command" >&2; exit 1; }
done
# A submodule uses a .git file while an external checkout commonly uses a
# .git directory; accept both forms and let the commit check below verify it.
[[ -e "$why_root/.git" && -e "$linux_root/.git" && -d "$buildroot_root" ]] || {
	echo "source root is incomplete; run tools/fetch-sources.sh first" >&2
	exit 1
}
if $network_profile && [[ ! -f "${hosted_root}/Makefile" ]]; then
	echo "M2 source root is missing ESP-Hosted-NG host/Makefile: ${hosted_root}" >&2
	exit 1
fi

python3 "${firmware_root}/tools/verify-source-lock.py"

locked_commit() {
	local key=$1
	python3 - "$lock_file" "$key" <<'PY'
import json, sys
lock, key = sys.argv[1:]
data = json.load(open(lock, encoding="utf-8"))
print(data["sources"][key]["commit"])
PY
}

assert_commit() {
	local key=$1 path=$2
	local expected actual
	expected=$(locked_commit "$key")
	actual=$(git -C "$path" rev-parse HEAD)
	[[ "$actual" == "$expected" ]] || {
		echo "$path is at $actual, expected locked $expected" >&2
		exit 1
	}
}

assert_commit why2025_linux_reference "$why_root"
assert_commit linux_base "$linux_root"
assert_commit buildroot "$buildroot_root"
linux_commit=$(locked_commit linux_base)
linux_tarball="${output}/linux-${linux_commit}.tar.gz"

# The stage is generated output. Recreate it so a retry cannot retain a
# reference patch from an earlier board-port attempt (notably the WHY2025 DTS
# hunk that EasyStick must never apply).
rm -rf -- "$patch_stage"
mkdir -p -- "$patch_stage"
if $c68_profile; then
	[[ -n "${EASYSTICK_KERNEL_PATCH_OVERRIDE_DIR:-}" ]] || {
		echo "C68_GATE_FAIL: use linux/build-c68.sh (nm-rendered 0038/0039 required)" >&2
		exit 1
	}
	override_dir=$(cd -- "${EASYSTICK_KERNEL_PATCH_OVERRIDE_DIR}" && pwd)
	patch_stage_abs=$(cd -- "$patch_stage" && pwd)
	[[ "$override_dir" != "$patch_stage_abs" ]] || {
		echo "C68_GATE_FAIL: rendered patches must not live in kernel-patches stage" >&2
		exit 1
	}
	for c68_patch in \
		0038-easystick-c68-clean-release-spinwait.patch \
		0039-easystick-c68-clean-release-smpboot.patch; do
		[[ -f "${override_dir}/${c68_patch}" ]] || {
			echo "C68_GATE_FAIL: missing rendered ${c68_patch}" >&2
			exit 1
		}
		if grep -q C68DEAD "${override_dir}/${c68_patch}"; then
			echo "C68_GATE_FAIL: placeholder remains in ${c68_patch}" >&2
			exit 1
		fi
	done
fi
if [[ "${EASYSTICK_CMD53_RETENTION_BB:-0}" == "1" ]]; then
	# The canonical 0054 context is generated from the real 0019 + 0035
	# source. Do not silently try to apply it to a profile that omits 0035.
	[[ "$profile" == "m3-lab" ]] || {
		echo "CMD53_BB_GATE_FAIL: retention BB 0054 requires --profile m3-lab (0019 + 0035)" >&2
		exit 1
	}
	[[ -n "${EASYSTICK_KERNEL_PATCH_OVERRIDE_DIR:-}" ]] || {
		echo "CMD53_BB_GATE_FAIL: use linux/build-cmd53-bb.sh (nm-rendered 0052 required)" >&2
		exit 1
	}
	override_dir=$(cd -- "${EASYSTICK_KERNEL_PATCH_OVERRIDE_DIR}" && pwd)
	[[ -f "${override_dir}/0052-easystick-dw-mmc-cmd53-retention-bb.patch" ]] || {
		echo "CMD53_BB_GATE_FAIL: missing rendered 0052" >&2
		exit 1
	}
	[[ -f "${override_dir}/0053-easystick-mmc-cmd53-post-bb.patch" ]] || {
		echo "CMD53_BB_GATE_FAIL: missing rendered 0053" >&2
		exit 1
	}
	if grep -q BBDEAD "${override_dir}/0052-easystick-dw-mmc-cmd53-retention-bb.patch"; then
		echo "CMD53_BB_GATE_FAIL: placeholder remains in rendered 0052" >&2
		exit 1
	fi
	if grep -q BBDEAD "${override_dir}/0053-easystick-mmc-cmd53-post-bb.patch"; then
		echo "CMD53_BB_GATE_FAIL: placeholder remains in rendered 0053" >&2
		exit 1
	fi
	# The normal Shot C contract must observe DMA, not ambient FORCE_PIO=1.
	# The only permitted exception is the explicit volume-native Linux-only PIO
	# A/B; reject any other bypass that would still apply 0008 / ledgers.
	if [[ "${EASYSTICK_SDIO_FORCE_PIO:-1}" != "0" ]]; then
		if [[ "$cmd53_bb_allow_pio" == "1" &&
		      "${EASYSTICK_LINUX_ONLY:-0}" == "1" ]]; then
			echo "CMD53_BB_AB: allowing FORCE_PIO=1 only for explicit Linux-only retention-BB A/B" >&2
		else
			echo "CMD53_BB_GATE_FAIL: EASYSTICK_SDIO_FORCE_PIO must be 0 for retention BB (DMA)" >&2
			exit 1
		fi
	fi
	if [[ "${EASYSTICK_IDMAC_DESC_INVALIDATE:-0}" != "0" ||
	      "${EASYSTICK_CMD53_RX_DESC_BYTES:-0}" != "0" ]]; then
		echo "CMD53_BB_GATE_FAIL: IDMAC invalidate / CMD53_RX_DESC A/B patches must be 0 for retention BB" >&2
		exit 1
	fi
	if [[ "${EASYSTICK_IDMAC_NONCOHERENT_RING:-0}" != "0" ]]; then
		# Observed 2026-08-21: NC=0 (cached/manual sync) fails M3-lab Wi-Fi
		# association (SIOCSIFFLAGS), so SSH /bin/true is unreachable. Allow
		# NC=1 only with an explicit override; still not FORCE_PIO.
		if [[ "${EASYSTICK_CMD53_BB_ALLOW_IDMAC_NC:-0}" == "1" ]]; then
			echo "CMD53_BB_WARN: IDMAC_NONCOHERENT_RING=1 with ALLOW_IDMAC_NC (Wi-Fi reachability override)" >&2
		else
			echo "CMD53_BB_GATE_FAIL: IDMAC_NONCOHERENT_RING must be 0 for retention BB (or set EASYSTICK_CMD53_BB_ALLOW_IDMAC_NC=1)" >&2
			exit 1
		fi
	fi
	if [[ "${EASYSTICK_ESPHOSTED_TX_LEDGER:-0}" != "0" ||
	      "${EASYSTICK_TCP22_LEDGER:-0}" != "0" ||
	      "${EASYSTICK_SSH_LEDGER:-0}" != "0" ||
	      "${EASYSTICK_ESPHOSTED_DIAGNOSTICS:-0}" != "0" ]]; then
		echo "CMD53_BB_GATE_FAIL: TX/TCP/SSH ledger and diagnostics must be 0 for retention BB" >&2
		exit 1
	fi
	[[ -n "${EASYSTICK_CMD53_BB_PA:-}" ]] || {
		echo "CMD53_BB_GATE_FAIL: EASYSTICK_CMD53_BB_PA unset" >&2
		exit 1
	}
	[[ -n "${EASYSTICK_CMD53_BB_0052_SHA256:-}" ]] || {
		echo "CMD53_BB_GATE_FAIL: EASYSTICK_CMD53_BB_0052_SHA256 unset" >&2
		exit 1
	}
	[[ -n "${EASYSTICK_CMD53_BB_0053_SHA256:-}" ]] || {
		echo "CMD53_BB_GATE_FAIL: EASYSTICK_CMD53_BB_0053_SHA256 unset" >&2
		exit 1
	}
fi
patches=(
	0001-riscv-esp32p4-baseline.patch
	0010-mm-nommu-userspace-pool.patch
	0014-riscv-signal-mcause-hardening.patch
	0015-riscv-esp32p4-cache-thunk-hardening.patch
	0019-watchdog-esp32p4-mwdt.patch
	0022-esp32p4-systimer-level-trigger.patch
	0023-riscv-esp32p4-force-mpie-user-return.patch
	0024-riscv-esp32p4-early-uart-marker.patch
		0025-esp32p4-usb-jtag-earlycon.patch
		0026-riscv-esp32p4-boot-markers.patch
		0027-easystick-headless-uart-console.patch
		0028-esp32p4-usb-serial-jtag-acm.patch
		0062-easystick-esp32p4-usb-acm-tx-bounded-poll.patch
	0031-riscv-esp32p4-m-mode-userspace.patch
)
if ! $c68_profile; then
	patches+=(0016-clocksource-esp32p4-systimer-hardening.patch)
fi
# m3-lab only: MWDT feed observe markers (timeout/policy unchanged).
# m3-lab only: TCP/22 bidirectional progress ledger (0024-C2; observe-only).
# It hooks hot TCP paths and emits KERN_EMERG records, so keep it opt-in:
# leaving it enabled during an SSH command can starve this NOMMU target's
# watchdog even though the patch is logically behavior-neutral.
if $lab_profile; then
	patches+=(0035-easystick-esp32p4-wdt-feed-observe.patch)
	if [[ "${EASYSTICK_TCP22_LEDGER:-0}" == "1" ]]; then
		patches+=(0036-easystick-tcp22-bidirectional-ledger.patch)
	fi
fi
# C67-L2-STAGE: observe-only SMP breadcrumbs in smpboot.c (no behaviour change).
if [[ "${EASYSTICK_C67_SMP_STAGE:-0}" == "1" || "${EASYSTICK_C68_CLEAN_RELEASE:-0}" == "1" ]]; then
	patches+=(0037-easystick-c67-l2-stage-smpboot.patch)
fi
# C68-CLEAN-RELEASE: Linux spinwait writes GO to boot-shim c68_release after bootdata.
if [[ "${EASYSTICK_C68_CLEAN_RELEASE:-0}" == "1" ]]; then
	patches+=(0038-easystick-c68-clean-release-spinwait.patch)
	patches+=(0039-easystick-c68-clean-release-smpboot.patch)
	patches+=(0040-easystick-esp32p4-ipi-provider.patch)
	patches+=(0041-easystick-esp32p4-ipi-driver.patch)
	patches+=(0042-easystick-esp32p4-systimer-cpu1.patch)
	if [[ "${EASYSTICK_C68_TIMER_OBSERVE:-0}" == "1" ]]; then
		patches+=(0044-easystick-esp32p4-systimer-observe-cpu1.patch)
	fi
		if [[ "${EASYSTICK_C68_TIMER_LOCK:-0}" == "1" ]]; then
			patches+=(0045-easystick-esp32p4-systimer-st-conf-lock.patch)
		fi
		if [[ "${EASYSTICK_C68_CONTEXT_OBSERVE:-0}" == "1" ]]; then
			patches+=(0046-easystick-esp32p4-cpu1-context-observe.patch)
		fi
		if [[ "${EASYSTICK_C68_ILLEGAL_OBSERVE:-0}" == "1" ]]; then
			patches+=(0047-easystick-esp32p4-cpu1-illegal-observe.patch)
		fi
		if [[ "${EASYSTICK_C68_L3_SMOKE:-0}" == "1" ]]; then
			patches+=(0048-easystick-esp32p4-l3-smoke-endpoint.patch)
			patches+=(0049-easystick-esp32p4-l3-smoke-workqueue.patch)
		fi
	if [[ "${EASYSTICK_TSENS_LINUX:-0}" == "1" ]]; then
		patches+=(0050-thermal-esp32p4-add-lp-tsens-driver.patch)
	fi
fi
if $network_profile; then
	patches+=(0002-gpio-esp32p4.patch)
	patches+=(0007-mmc-dw_mmc-esp32p4-fixes.patch)
	# PIO is the conservative default while the NOMMU DMA ring is under
	# validation.  Set EASYSTICK_SDIO_FORCE_PIO=0 for the board-side DMA
	# comparison; all other transport patches remain identical.
	if [[ "${EASYSTICK_SDIO_FORCE_PIO:-1}" != "0" ]]; then
		patches+=(0008-easystick-dw-mmc-force-pio.patch)
	fi
	patches+=(0009-easystick-dw-mmc-divider0.patch)
	patches+=(0010-easystick-dw-mmc-cmd53-status.patch)
	patches+=(0011-easystick-dw-mmc-idmac-rx-diagnostics.patch)
	# Descriptor-coherency A/B.  Off by default so the confirmed no-0010 DMA
	# baseline is what an unqualified build reproduces; set
	# EASYSTICK_IDMAC_DESC_INVALIDATE=1 for the single-variable experiment
	# that invalidates one descriptor cache line before each des0 re-read.
	if [[ "${EASYSTICK_IDMAC_DESC_INVALIDATE:-0}" == "1" ]]; then
		patches+=(0012-easystick-dw-mmc-idmac-desc-invalidate.patch)
	fi
	# Production descriptor-ring fix.  Replaces the devm_kzalloc +
	# virt_to_phys fallback with dma_alloc_noncoherent() and explicit
	# bidirectional ownership transitions.  It rewrites the same OWN polls
	# 0012 rewrites, so the two are mutually exclusive by construction.
	if [[ "${EASYSTICK_IDMAC_NONCOHERENT_RING:-0}" == "1" ]]; then
		if [[ "${EASYSTICK_IDMAC_DESC_INVALIDATE:-0}" == "1" ]]; then
			echo "EASYSTICK_IDMAC_NONCOHERENT_RING and EASYSTICK_IDMAC_DESC_INVALIDATE are mutually exclusive" >&2
			exit 1
		fi
		patches+=(0013-easystick-dw-mmc-idmac-noncoherent-ring.patch)
	fi
	# CMD53-RX descriptor-chunk diagnostic.  Stacks on the 0013 noncoherent
	# ring (it edits the same descriptor-fill helper 0013 introduces), so
	# it requires 0013 rather than being independently selectable.  Its own
	# runtime parameter, easystick_cmd53_rx_desc_bytes, defaults to 0 and
	# must be left at that default here; this variable only controls
	# whether the patch is staged into the kernel source at all.
	if [[ "${EASYSTICK_CMD53_RX_DESC_BYTES:-0}" == "1" ]]; then
		if [[ "${EASYSTICK_IDMAC_NONCOHERENT_RING:-0}" != "1" ]]; then
			echo "EASYSTICK_CMD53_RX_DESC_BYTES requires EASYSTICK_IDMAC_NONCOHERENT_RING=1" >&2
			exit 1
		fi
		patches+=(0014-easystick-dw-mmc-idmac-cmd53-rx-desc-bytes.patch)
	fi
	# Experiment B: error-only CMD53 / DW-MMC provenance capsule.  Replaces
	# the 0010 per-data-error printk with one ES_MMC record after failed
	# request completion (cmd_err/data_err/status/idsts/elapsed_us).
	# Successful CMD53 stays silent.  Off by default.
	if [[ "${EASYSTICK_DW_MMC_CMD53_ERR_PROV:-0}" == "1" ]]; then
		if [[ "${EASYSTICK_CMD53_RETENTION_BB:-0}" == "1" ]]; then
			echo "EASYSTICK_DW_MMC_CMD53_ERR_PROV and EASYSTICK_CMD53_RETENTION_BB are mutually exclusive" >&2
			exit 1
		fi
		patches+=(0051-easystick-dw-mmc-cmd53-err-provenance.patch)
	fi
	# Experiment C/0053: retention black-box.  Template keeps 0xBBDEAD01u; only
	# nm-rendered copies from build-cmd53-bb.sh may be applied.
	if [[ "${EASYSTICK_CMD53_RETENTION_BB:-0}" == "1" ]]; then
		if [[ -z "${EASYSTICK_KERNEL_PATCH_OVERRIDE_DIR:-}" ]]; then
			echo "CMD53_BB_GATE_FAIL: use linux/build-cmd53-bb.sh (nm-rendered 0052/0053/0054 required)" >&2
			exit 1
		fi
		patches+=(0052-easystick-dw-mmc-cmd53-retention-bb.patch)
		patches+=(0053-easystick-mmc-cmd53-post-bb.patch)
		if [[ "${EASYSTICK_WDT_CRASH_CAPSULE:-1}" == "1" ]]; then
			patches+=(0054-easystick-wdt-crash-capsule.patch)
		else
			echo "CMD53_BB_WARN: skipping 0054 crash capsule (EASYSTICK_WDT_CRASH_CAPSULE=0)" >&2
		fi
		if [[ "$stacktrace_diagnostics" == "1" ]]; then
			patches+=(0056-easystick-wdt-stacktrace.patch)
		fi
		if [[ "${EASYSTICK_WDT_BEAT_BREADCRUMB:-1}" == "1" ]]; then
			if [[ "${EASYSTICK_WDT_CRASH_CAPSULE:-1}" == "1" ]]; then
				patches+=(0057-easystick-wdt-beat-breadcrumb.patch)
			else
				# 0057 depends on 0054 crash-capsule context. Instrument C
				# on the 184.5s epoch uses 0058 against 0019+0035 only.
				patches+=(0058-easystick-wdt-beat-nocapsule.patch)
			fi
		else
			echo "CMD53_BB_WARN: skipping 0057/0058 beat breadcrumb (EASYSTICK_WDT_BEAT_BREADCRUMB=0)" >&2
		fi
		if [[ "$wdt_inject" == "1" ]]; then
			patches+=(0059-easystick-wdt-inject.patch)
		else
			echo "CMD53_BB_WARN: skipping 0059 WDT injector (EASYSTICK_WDT_INJECT=0)" >&2
		fi
	fi
	patches+=(0055-easystick-sdio-clock-attest.patch)
	# CMD53 write that gets CMD_DONE but never DATA_OVER: arm the existing
	# software DTO timer for SEND as well as RECV (recv-only left TX unbounded).
	patches+=(0060-easystick-dw-mmc-tx-software-dto.patch)
	# 0060 only arms from BH wait sites. If BH does not re-enter after
	# CMD_DONE, arm the same timer from the write CMD_DONE IRQ instead.
	patches+=(0061-easystick-dw-mmc-tx-dto-on-cmd-done.patch)
fi
for patch_name in "${patches[@]}"; do
	if [[ "$patch_name" == 0022-esp32p4-systimer-level-trigger.patch ||
	      "$patch_name" == 0024-riscv-esp32p4-early-uart-marker.patch ||
	      "$patch_name" == 0025-esp32p4-usb-jtag-earlycon.patch ||
	      "$patch_name" == 0026-riscv-esp32p4-boot-markers.patch ||
	      "$patch_name" == 0027-easystick-headless-uart-console.patch ||
	      "$patch_name" == 0028-esp32p4-usb-serial-jtag-acm.patch ||
	      "$patch_name" == 0062-easystick-esp32p4-usb-acm-tx-bounded-poll.patch ||
	      "$patch_name" == 0008-easystick-dw-mmc-force-pio.patch ||
	      "$patch_name" == 0009-easystick-dw-mmc-divider0.patch ||
	      "$patch_name" == 0010-easystick-dw-mmc-cmd53-status.patch ||
	      "$patch_name" == 0011-easystick-dw-mmc-idmac-rx-diagnostics.patch ||
	      "$patch_name" == 0012-easystick-dw-mmc-idmac-desc-invalidate.patch ||
	      "$patch_name" == 0013-easystick-dw-mmc-idmac-noncoherent-ring.patch ||
	      "$patch_name" == 0014-easystick-dw-mmc-idmac-cmd53-rx-desc-bytes.patch ||
	      "$patch_name" == 0051-easystick-dw-mmc-cmd53-err-provenance.patch ||
	      "$patch_name" == 0052-easystick-dw-mmc-cmd53-retention-bb.patch ||
	      "$patch_name" == 0053-easystick-mmc-cmd53-post-bb.patch ||
	      "$patch_name" == 0054-easystick-wdt-crash-capsule.patch ||
	      "$patch_name" == 0057-easystick-wdt-beat-breadcrumb.patch ||
	      "$patch_name" == 0058-easystick-wdt-beat-nocapsule.patch ||
	      "$patch_name" == 0059-easystick-wdt-inject.patch ||
	      "$patch_name" == 0056-easystick-wdt-stacktrace.patch ||
	      "$patch_name" == 0055-easystick-sdio-clock-attest.patch ||
	      "$patch_name" == 0060-easystick-dw-mmc-tx-software-dto.patch ||
	      "$patch_name" == 0061-easystick-dw-mmc-tx-dto-on-cmd-done.patch ||
	      "$patch_name" == 0035-easystick-esp32p4-wdt-feed-observe.patch ||
	      "$patch_name" == 0036-easystick-tcp22-bidirectional-ledger.patch ||
	      "$patch_name" == 0037-easystick-c67-l2-stage-smpboot.patch ||
	      "$patch_name" == 0038-easystick-c68-clean-release-spinwait.patch ||
	      "$patch_name" == 0039-easystick-c68-clean-release-smpboot.patch ||
	      "$patch_name" == 0040-easystick-esp32p4-ipi-provider.patch ||
	      "$patch_name" == 0041-easystick-esp32p4-ipi-driver.patch ||
	      "$patch_name" == 0042-easystick-esp32p4-systimer-cpu1.patch ||
	      "$patch_name" == 0044-easystick-esp32p4-systimer-observe-cpu1.patch ||
	      "$patch_name" == 0045-easystick-esp32p4-systimer-st-conf-lock.patch ||
	      "$patch_name" == 0046-easystick-esp32p4-cpu1-context-observe.patch ||
	      "$patch_name" == 0047-easystick-esp32p4-cpu1-illegal-observe.patch ||
	      "$patch_name" == 0048-easystick-esp32p4-l3-smoke-endpoint.patch ||
	      "$patch_name" == 0049-easystick-esp32p4-l3-smoke-workqueue.patch ||
	      "$patch_name" == 0050-thermal-esp32p4-add-lp-tsens-driver.patch ]]; then
		src="${firmware_root}/linux/kernel-patches/${patch_name}"
	else
		src="${why_root}/patches/linux/${patch_name}"
	fi
	if [[ -f "$src" ]]; then
		override_src="${EASYSTICK_KERNEL_PATCH_OVERRIDE_DIR:-}/${patch_name}"
		if [[ -n "${EASYSTICK_KERNEL_PATCH_OVERRIDE_DIR:-}" && -f "$override_src" ]]; then
			cp -- "$override_src" "${patch_stage}/${patch_name}"
		else
			cp -- "$src" "${patch_stage}/${patch_name}"
		fi
	else
		# The locked WHY2025 submodule is intentionally sparse in a normal
		# checkout.  Read the patch from its Git object instead of requiring
		# callers to mutate submodule sparse-checkout state.
		[[ "$src" == "${why_root}/"* ]] || {
			echo "missing project patch: $src" >&2
			exit 1
		}
		git -C "$why_root" show "HEAD:patches/linux/${patch_name}" \
			>"${patch_stage}/${patch_name}" || {
			echo "missing reference patch in locked Git object: $src" >&2
			exit 1
		}
	fi
done
printf '%s\n' "why2025 reference commit: $(git -C "$why_root" rev-parse HEAD)" >"${patch_stage}/SOURCE.txt"
printf '%s\n' "selected baseline patches: ${patches[*]}" >>"${patch_stage}/SOURCE.txt"
printf '%s\n' "sdio-default-mhz: ${sdio_default_mhz}" >>"${patch_stage}/SOURCE.txt"
printf '%s\n' "wdt-test-timeout-s: ${wdt_test_timeout_s}" >>"${patch_stage}/SOURCE.txt"
printf '%s\n' "wdt-test-no-feed: ${wdt_test_no_feed}" >>"${patch_stage}/SOURCE.txt"
printf '%s\n' "stacktrace-diagnostics: ${stacktrace_diagnostics}" >>"${patch_stage}/SOURCE.txt"
printf '%s\n' "esp-hosted-cmd52-trace: ${cmd52_trace}" >>"${patch_stage}/SOURCE.txt"
printf '%s\n' "esp-hosted-cmd52-marker: ${cmd52_marker}" >>"${patch_stage}/SOURCE.txt"
printf '%s\n' "esp-hosted-cmd52-shot-gate: ${cmd52_marker}" >>"${patch_stage}/SOURCE.txt"

# Linux 6.12+ expects out-of-tree DTS overlays to retain their vendor
# directory and Makefile.  Keep the board DTS single-sourced in Git, then
# materialize that tiny vendor tree in generated output for Buildroot.
rm -rf -- "$dts_stage"
mkdir -p -- "${dts_stage}/espressif"
cp -- "${firmware_root}/linux/dts/easystick-stamp-p4.dts" \
	"${dts_stage}/espressif/easystick-stamp-p4.dts"
if [[ "$wdt_test_timeout_s" != "0" ]]; then
	python3 - "${dts_stage}/espressif/easystick-stamp-p4.dts" \
		"$wdt_test_timeout_s" "$wdt_test_no_feed" <<'PY'
from pathlib import Path
import sys

path = Path(sys.argv[1])
timeout, no_feed = sys.argv[2:]
text = path.read_text(encoding="utf-8")
needle = ' idle=poll";'
params = f' esp32p4_wdt.test_timeout_s={timeout}'
if no_feed == "1":
    params += " esp32p4_wdt.test_no_feed=1"
replacement = f' idle=poll{params}";'
if text.count(needle) != 1:
    raise SystemExit(
        "WDT_TEST_GATE_FAIL: expected exactly one unmodified bootargs terminator"
    )
path.write_text(text.replace(needle, replacement, 1), encoding="utf-8", newline="\n")
PY
fi
cp -- "${firmware_root}/linux/linux.config" "${kernel_config_fragment}"
grep -Fqx -- "CONFIG_SMP=n" "${firmware_root}/linux/linux.config" || {
	echo "UP kernel config gate failed: source linux.config must keep CONFIG_SMP=n" >&2
	exit 1
}
if grep -q 'cpu@1' "${firmware_root}/linux/dts/easystick-stamp-p4.dts"; then
	echo "UP DTS gate failed: source easystick-stamp-p4.dts must not contain cpu@1" >&2
	exit 1
fi
if $network_profile; then
	cat "${firmware_root}/linux/m2/kernel.config.fragment" >>"${kernel_config_fragment}"
fi
if [[ "$stacktrace_diagnostics" == "1" ]]; then
	cat >>"${kernel_config_fragment}" <<'EOF'

# P4-only stacktrace/CMD52 diagnostic image.
CONFIG_STACKTRACE=y
CONFIG_KALLSYMS=y
CONFIG_KALLSYMS_ALL=y
CONFIG_DEBUG_INFO_DWARF4=y
CONFIG_FRAME_POINTER=y
EOF
fi
if $c68_profile; then
	cat "${firmware_root}/linux/c68/linux.config.fragment" >>"${kernel_config_fragment}"
	if $c68_top_profile; then
		cat >>"${kernel_config_fragment}" <<'EOF'

# C68 top/stress profiles need a live ttyGS1 askfirst shell on COM10.
CONFIG_SERIAL_ESP32_ACM=y
EOF
	fi
	if [[ "${EASYSTICK_TSENS_LINUX:-0}" == "1" ]]; then
		cat >>"${kernel_config_fragment}" <<'EOF'

# Experimental ESP32-P4 LP-TSENS bring-up.
CONFIG_THERMAL=y
CONFIG_THERMAL_OF=y
CONFIG_ESP32P4_TSENS=y
EOF
	fi
	python3 "${firmware_root}/linux/c68/insert_cpu1.py" \
		"${dts_stage}/espressif/easystick-stamp-p4.dts" \
		"${firmware_root}/linux/c68/cpu1.dts.inc" \
		"${firmware_root}/linux/c68/ipi.dts.inc"
else
	if grep -q 'cpu@1' "${dts_stage}/espressif/easystick-stamp-p4.dts"; then
		echo "UP DTS gate failed: staged DTB source contains cpu@1" >&2
		exit 1
	fi
	if grep -Fqx -- "CONFIG_SMP=y" "${kernel_config_fragment}"; then
		echo "UP kernel config gate failed: staged fragment enabled SMP" >&2
		exit 1
	fi
fi
cat >"${dts_stage}/espressif/Makefile" <<'EOF'
# SPDX-License-Identifier: GPL-2.0-only
dtb-$(CONFIG_ARCH_ESPRESSIF) += easystick-stamp-p4.dtb
EOF

make_args=(
	-C "$buildroot_for_make"
	O="$br_output"
	BR2_EXTERNAL="$external_root"
	BR2_LINUX_KERNEL_PATCH="$patch_stage"
	BR2_LINUX_KERNEL_CUSTOM_DTS_DIR="$dts_stage"
	BR2_LINUX_KERNEL_CONFIG_FRAGMENT_FILES="$kernel_config_fragment"
	BR2_LINUX_KERNEL_CUSTOM_TARBALL=y
	BR2_LINUX_KERNEL_CUSTOM_TARBALL_LOCATION="file://${linux_tarball}"
)
if [[ -n ${EASYSTICK_CCACHE_DIR:-} ]]; then
	# Keep cache storage outside the repository and make the location explicit
	# for container/CI callers.  Without this opt-in, Buildroot's default is
	# the invoking user's home directory and is often ephemeral in a container.
	export BR2_CCACHE_DIR="${EASYSTICK_CCACHE_DIR}"
	make_args+=(BR2_CCACHE_DIR="${EASYSTICK_CCACHE_DIR}")
fi
if $network_profile; then
	# The package source is selected from the same locked source root as
	# Buildroot.  This keeps the profile usable with either --vendor or the
	# external source cache while keeping the upstream tree out of Git.
	make_args+=(ESP_HOSTED_NG_SITE="${hosted_root}")
	make_args+=(BR2_DL_DIR="${buildroot_root}/dl")
fi

if [[ -n ${EASYSTICK_BUILDROOT_JLEVEL:-} ]]; then
	case "$EASYSTICK_BUILDROOT_JLEVEL" in
		''|*[!0-9]*|0)
			echo "EASYSTICK_BUILDROOT_JLEVEL must be a positive integer" >&2
			exit 2
			;;
	esac
	make_args+=("BR2_JLEVEL=${EASYSTICK_BUILDROOT_JLEVEL}")
fi

# Retention BB PA / rendered-0052 digest must be part of identity so a reused
# Buildroot tree cannot keep a kernel built against a previous nm PA.
cmd53_bb_pa_id="${EASYSTICK_CMD53_BB_PA:-none}"
cmd53_bb_0052_sha_id="${EASYSTICK_CMD53_BB_0052_SHA256:-none}"
cmd53_bb_0053_sha_id="${EASYSTICK_CMD53_BB_0053_SHA256:-none}"
cmd53_bb_0054_sha_id="${EASYSTICK_CMD53_BB_0054_SHA256:-none}"
profile_identity="profile-contract=21 profile=${profile} network=${network_profile} ssh=${ssh_profile} lab=${lab_profile} sdio-default-mhz=${sdio_default_mhz} wdt-test-timeout-s=${wdt_test_timeout_s} wdt-test-no-feed=${wdt_test_no_feed} stacktrace-diagnostics=${stacktrace_diagnostics} stacktrace-module-debug-strip=${stacktrace_diagnostics} esp-hosted-cmd52-trace=${cmd52_trace} esp-hosted-cmd52-marker=${cmd52_marker} ssh-ledger=${EASYSTICK_SSH_LEDGER:-0} tcp22-ledger=${EASYSTICK_TCP22_LEDGER:-0} esp-hosted-tx-ledger=${EASYSTICK_ESPHOSTED_TX_LEDGER:-0} esp-hosted-diagnostics=${EASYSTICK_ESPHOSTED_DIAGNOSTICS:-0} dw-mmc-cmd53-err-prov=${EASYSTICK_DW_MMC_CMD53_ERR_PROV:-0} cmd53-retention-bb=${EASYSTICK_CMD53_RETENTION_BB:-0} cmd53-bb-allow-pio=${cmd53_bb_allow_pio} cmd53-bb-pa=${cmd53_bb_pa_id} cmd53-bb-0052-sha=${cmd53_bb_0052_sha_id} cmd53-bb-0053-sha=${cmd53_bb_0053_sha_id} cmd53-bb-0054-sha=${cmd53_bb_0054_sha_id} force-pio=${EASYSTICK_SDIO_FORCE_PIO:-1} idmac-inv=${EASYSTICK_IDMAC_DESC_INVALIDATE:-0} idmac-nc=${EASYSTICK_IDMAC_NONCOHERENT_RING:-0} cmd53-rx-desc=${EASYSTICK_CMD53_RX_DESC_BYTES:-0} buildroot-jlevel=${EASYSTICK_BUILDROOT_JLEVEL:-auto} c68=${c68_profile} top=${c68_top_profile} stress=${c68_stress_profile} l3=${EASYSTICK_C68_L3_SMOKE:-0} tsens-linux=${EASYSTICK_TSENS_LINUX:-0} tsens-oracle=${EASYSTICK_TSENS_ORACLE:-0}"

set_config_value() {
	local config=$1 key=$2 value=$3
	python3 - "$config" "$key" "$value" <<'PY'
from pathlib import Path
import sys

config, key, value = sys.argv[1:]
path = Path(config)
lines = path.read_text(encoding="utf-8").splitlines()
prefixes = (f"{key}=", f"# {key} is not set")
replacement = value if value.startswith("# ") else f"{key}={value}"
out = []
inserted = False
for line in lines:
    if line.startswith(prefixes):
        if not inserted:
            out.append(replacement)
            inserted = True
        continue
    out.append(line)
if not inserted:
    out.append(replacement)
path.write_text("\n".join(out) + "\n", encoding="utf-8")
PY
}

contains_c68dead() {
	local path=$1
	python3 - "$path" <<'PY'
from pathlib import Path
import sys
data = Path(sys.argv[1]).read_bytes()
raise SystemExit(0 if b"C68DEAD" in data else 1)
PY
}

contains_binary_text() {
	local path=$1 marker=$2
	python3 - "$path" "$marker" <<'PY'
from pathlib import Path
import sys

path, marker = sys.argv[1:]
raise SystemExit(0 if marker.encode() in Path(path).read_bytes() else 1)
PY
}

wdt_inject_fail_closed_after_linux() {
	[[ "${EASYSTICK_CMD53_RETENTION_BB:-0}" == "1" ]] || return 0
	[[ "${EASYSTICK_WDT_INJECT:-1}" == "1" ]] || {
		echo "CMD53_BB_WARN: skipping 0059 inject gate (EASYSTICK_WDT_INJECT=0)" >&2
		return 0
	}
	local source="${br_output}/build/linux-custom/drivers/watchdog/esp32p4_wdt.c"
	local vmlinux="${br_output}/build/linux-custom/vmlinux"
	[[ -f "$source" && -f "$vmlinux" ]] || {
		echo "WDT_INJECT_GATE_FAIL: missing $source or $vmlinux" >&2
		exit 1
	}
	contains_binary_text "$source" 'easystick_wdt_inject_spin' || {
		echo "WDT_INJECT_GATE_FAIL: easystick_wdt_inject_spin missing from source" >&2
		exit 1
	}
	contains_binary_text "$source" 'local_irq_disable();' || {
		echo "WDT_INJECT_GATE_FAIL: local_irq_disable missing from inject source" >&2
		exit 1
	}
	contains_binary_text "$vmlinux" 'EASYSTICK_WDT INJECT' || {
		echo "WDT_INJECT_GATE_FAIL: INJECT banner missing from vmlinux" >&2
		exit 1
	}
}

wdt_crash_fail_closed_after_linux() {
	[[ "${EASYSTICK_CMD53_RETENTION_BB:-0}" == "1" ]] || return 0
	if [[ "${EASYSTICK_WDT_CRASH_CAPSULE:-1}" != "1" ]]; then
		echo "CMD53_BB_WARN: skipping 0054 fail-closed gate (EASYSTICK_WDT_CRASH_CAPSULE=0)" >&2
		return 0
	fi

	local patch="${patch_stage}/0054-easystick-wdt-crash-capsule.patch"
	local source="${br_output}/build/linux-custom/drivers/watchdog/esp32p4_wdt.c"
	local vmlinux="${br_output}/build/linux-custom/vmlinux"
	local image="${br_output}/images/Image"
	local attest="${output}/wdt-crash-capsule-attestation.json"
	local expected_patch_sha="${EASYSTICK_CMD53_BB_0054_SHA256:-}"
	local expected_bb_pa="${EASYSTICK_CMD53_BB_PA:-}"
	local nm="${br_output}/host/bin/riscv32-buildroot-linux-uclibc-nm"
	local objdump="${br_output}/host/bin/riscv32-buildroot-linux-uclibc-objdump"

	for required in "$patch" "$source" "$vmlinux" "$image"; do
		[[ -f "$required" ]] || {
			echo "WDT_CAPSULE_GATE_FAIL: missing ${required}" >&2
			exit 1
		}
	done
	[[ "$expected_patch_sha" =~ ^[[:xdigit:]]{64}$ ]] || {
		echo "WDT_CAPSULE_GATE_FAIL: missing or malformed rendered 0054 SHA256" >&2
		exit 1
	}
	[[ -n "$expected_bb_pa" ]] || {
		echo "WDT_CAPSULE_GATE_FAIL: missing CMD53 retention PA" >&2
		exit 1
	}
	[[ -x "$nm" && -x "$objdump" ]] || {
		echo "WDT_CAPSULE_GATE_FAIL: cross nm/objdump is missing" >&2
		exit 1
	}

	# The source gate below asserts that easystick_capture_crash, the
	# PRETIMEOUT pr_emerg and dump_stack() are all PRESENT.  Presence was
	# never the problem.  The 2026-08-28 S0 shot lost its capsule because the
	# print came FIRST and the handler had only 1.000 s before the stage-1
	# reset, so an empty capsule could not distinguish an IRQ-off hard lock
	# from a printk/console deadlock.  ORDER is the invariant and nothing
	# checked it (CLAUDE.md 14.2).  Assert it separately, with the required
	# grace cap stated HERE on the requirement side rather than read back out
	# of the artifact under test (CLAUDE.md 14.16).
	#
	# required_grace_cap is in MWDT ticks of 1 ms.  30000 puts the stage-0
	# pretimeout IRQ 30.000 s ahead of the stage-1 reset, so the handler is no
	# longer racing a 1.000 s window while it writes a capsule and, in the
	# diagnostic image, a dump_stack() to a 115200-baud console.  The stage-1
	# reset deadline itself is unchanged.
	local required_grace_cap=30000
	python3 "${firmware_root}/linux/cmd53-bb/assert_pretimeout_order.py" \
		--source "$source" \
		--stacktrace "$stacktrace_diagnostics" \
		--expect-grace-cap "$required_grace_cap" || {
		echo "WDT_CAPSULE_GATE_FAIL: pretimeout capsule-before-print ordering or grace cap assertion failed" >&2
		exit 1
	}

	python3 - "$patch" "$source" "$vmlinux" "$image" "$attest" \
		"$expected_patch_sha" "$expected_bb_pa" "$nm" "$objdump" <<'PY'
import hashlib
import json
import os
import subprocess
import sys
from pathlib import Path

(
    patch_path,
    source_path,
    vmlinux_path,
    image_path,
    attest_path,
    expected_patch_sha,
    expected_bb_pa,
    nm_path,
    objdump_path,
) = sys.argv[1:]


def sha256(path: str) -> str:
    digest = hashlib.sha256()
    with open(path, "rb") as stream:
        for chunk in iter(lambda: stream.read(1024 * 1024), b""):
            digest.update(chunk)
    return digest.hexdigest()


def fail(message: str) -> "NoReturn":
    raise SystemExit(f"WDT_CAPSULE_GATE_FAIL: {message}")


actual_patch_sha = sha256(patch_path)
if actual_patch_sha.lower() != expected_patch_sha.lower():
    fail(
        "rendered 0054 SHA mismatch "
        f"(expected {expected_patch_sha}, got {actual_patch_sha})"
    )

source = Path(source_path).read_text(encoding="utf-8", errors="replace")
required_source = (
    "EASYSTICK_CRASH_CAPSULE_MAGIC",
    "TIMG_WDT_STG1",
    "FIELD_PREP(TIMG_WDT_STG0, TIMG_WDT_STG_SEL_INT)",
    "FIELD_PREP(TIMG_WDT_STG1, TIMG_WDT_STG_SEL_RESET_SYSTEM)",
    "writel(pretimeout_ticks, wdt->base + TIMG_WDTCONFIG2)",
    "writel(timeout_ticks, wdt->base + TIMG_WDTCONFIG3)",
    "writel(int_ena | TIMG_WDT_INT, wdt->base + TIMG_INT_ENA_TIMERS)",
    "writel(int_ena & ~TIMG_WDT_INT, wdt->base + TIMG_INT_ENA_TIMERS)",
    "devm_request_irq(dev, wdt->irq, esp32p4_wdt_pretimeout",
    "easystick_capture_crash",
    "module_param_named(test_no_feed",
    "if (easystick_wdt_test_no_feed)",
    'pr_emerg("EASYSTICK_WDT PRETIMEOUT',
    'pr_emerg("EASYSTICK_WDT CAPSULE_COMMIT',
)
if os.environ.get("EASYSTICK_STACKTRACE_DIAGNOSTICS", "0") == "1":
    required_source += ("dump_stack();",)
missing_source = [marker for marker in required_source if marker not in source]
if missing_source:
    fail("source implementation markers missing: " + ", ".join(missing_source))
if "Stages 1-3 stay OFF" in source:
    fail("old stage-0-only watchdog implementation remains in source")
if expected_bb_pa not in source:
    fail("rendered CMD53 retention PA is absent from the built source")

nm = subprocess.run(
    [nm_path, "--defined-only", "-n", vmlinux_path],
    check=False,
    capture_output=True,
    text=True,
)
if nm.returncode:
    fail(f"nm failed with exit {nm.returncode}")
symbol_names = (
    "esp32p4_wdt_arm",
    "esp32p4_wdt_pretimeout",
    "easystick_capture_crash",
)
nm_lines = nm.stdout.splitlines()
missing_symbols = [
    name
    for name in symbol_names
    if not any(line.split() and line.split()[-1] == name for line in nm_lines)
]
if missing_symbols:
    fail("vmlinux symbols missing: " + ", ".join(missing_symbols))

disassembly = {}
for name in symbol_names:
    result = subprocess.run(
        [objdump_path, "-d", f"--disassemble={name}", vmlinux_path],
        check=False,
        capture_output=True,
        text=True,
    )
    if result.returncode or f"<{name}>:" not in result.stdout:
        fail(f"vmlinux disassembly missing {name}")
    disassembly[name] = True

payload = {
    "schema": 1,
    "kind": "easystick-wdt-crash-capsule-build-attestation",
    "patch": {
        "path": patch_path,
        "sha256": actual_patch_sha,
    },
    "source": {
        "path": source_path,
        "sha256": sha256(source_path),
        "markers": list(required_source),
    },
    "vmlinux": {
        "path": vmlinux_path,
        "sha256": sha256(vmlinux_path),
        "symbols": list(symbol_names),
        "disassembly": disassembly,
    },
    "Image": {
        "path": image_path,
        "sha256": sha256(image_path),
    },
    "cmd53_retention_pa": expected_bb_pa,
}
Path(attest_path).write_text(
    json.dumps(payload, indent=2) + "\n",
    encoding="utf-8",
)
PY
	echo "WDT crash-capsule fail-closed gate passed: ${attest}"
}

c68_fail_closed_after_linux() {
	local kernel_config="${br_output}/build/linux-custom/.config"
	local image="${br_output}/images/Image"
	local dtb="${br_output}/images/easystick-stamp-p4.dtb"
	local vmlinux="${br_output}/build/linux-custom/vmlinux"
	local dtc="${br_output}/host/bin/dtc"
	[[ -f "$kernel_config" ]] || {
		echo "C68_GATE_FAIL: missing $kernel_config" >&2
		exit 1
	}
	if $c68_profile; then
		for expected in \
			"CONFIG_SMP=y" \
			"CONFIG_NR_CPUS=2" \
			"CONFIG_RISCV_BOOT_SPINWAIT=y" \
			"CONFIG_ESP32P4_IPI=y"; do
			grep -Fqx -- "${expected}" "$kernel_config" || {
				echo "C68_GATE_FAIL: kernel .config missing ${expected}" >&2
				exit 1
			}
		done
		[[ -f "$image" && -f "$dtb" && -f "$vmlinux" ]] || {
			echo "C68_GATE_FAIL: missing Image/DTB/vmlinux" >&2
			exit 1
		}
		if $c68_top_profile; then
			grep -Fqx -- "CONFIG_SERIAL_ESP32_ACM=y" "$kernel_config" || {
				echo "C68_GATE_FAIL: top/stress profile has no ttyGS1 ACM console" >&2
				exit 1
			}
		fi
		if [[ "${EASYSTICK_TSENS_LINUX:-0}" == "1" ]]; then
			for expected in \
				"CONFIG_THERMAL=y" \
				"CONFIG_THERMAL_OF=y" \
				"CONFIG_ESP32P4_TSENS=y"; do
				grep -Fqx -- "${expected}" "$kernel_config" || {
					echo "C68_GATE_FAIL: TSENS Linux build missing ${expected}" >&2
					exit 1
				}
			done
			grep -Fq 'REGI2C DAC readback' \
				"${patch_stage}/0050-thermal-esp32p4-add-lp-tsens-driver.patch" || {
				echo "C68_GATE_FAIL: TSENS patch has no REGI2C readback gate" >&2
				exit 1
			}
		fi
		[[ -x "$dtc" ]] || dtc=$(command -v dtc || true)
		[[ -n "$dtc" && -x "$dtc" ]] || {
			echo "C68_GATE_FAIL: dtc missing for DTB decompile" >&2
			exit 1
		}
		"$dtc" -I dtb -O dts "$dtb" | grep -q 'cpu@1' || {
			echo "C68_GATE_FAIL: decompiled DTB has no cpu@1" >&2
			exit 1
		}
		"$dtc" -I dtb -O dts "$dtb" | grep -q 'interrupt-controller' || {
			echo "C68_GATE_FAIL: decompiled DTB has no interrupt-controller" >&2
			exit 1
		}
		"$dtc" -I dtb -O dts "$dtb" | grep -q 'espressif,esp32p4-ipi' || {
			echo "C68_GATE_FAIL: decompiled DTB has no ESP32-P4 IPI provider" >&2
			exit 1
		}
		"$dtc" -I dtb -O dts "$dtb" |
			grep -A 10 'timer@500e2000' | grep -Fq '0x17 0x36' || {
			echo "C68_GATE_FAIL: decompiled DTB has no CPU1 SYSTIMER route" >&2
			exit 1
		}
		if [[ "${EASYSTICK_TSENS_LINUX:-0}" == "1" ]]; then
			"$dtc" -I dtb -O dts "$dtb" |
				grep -Fq 'espressif,esp32p4-tsens' || {
				echo "C68_GATE_FAIL: decompiled DTB has no ESP32-P4 TSENS node" >&2
				exit 1
			}
		fi
		if [[ "${EASYSTICK_C68_L3_SMOKE:-0}" == "1" ]]; then
			[[ -x "${br_output}/target/usr/sbin/m5stamp-smp-smoke" ]] || {
				echo "C68_GATE_FAIL: L3 smoke helper is missing from target rootfs" >&2
				exit 1
			}
			contains_binary_text "$vmlinux" "m5stamp_smp_smoke" || {
				echo "C68_GATE_FAIL: L3 smoke endpoint is not present in the built kernel" >&2
				exit 1
			}
			for smoke_helper in "${br_output}/target/usr/sbin/"*smp-smoke; do
				[[ -e "$smoke_helper" ]] || continue
				[[ "$(basename "$smoke_helper")" == "m5stamp-smp-smoke" ]] || {
					echo "C68_GATE_FAIL: stale non-M5Stamp SMP smoke helper in target rootfs: $(basename "$smoke_helper")" >&2
					exit 1
				}
			done
		fi
		if $c68_stress_profile && [[ "${EASYSTICK_LINUX_ONLY:-0}" != "1" ]]; then
			[[ -x "${br_output}/target/usr/sbin/m5stamp-smp-stress" ]] || {
				echo "C68_GATE_FAIL: SMP stress helper is missing from target rootfs" >&2
				exit 1
			}
		fi
		local ipi_driver_patch="${patch_stage}/0041-easystick-esp32p4-ipi-driver.patch"
		grep -Fq 'riscv_ipi_set_virq_range' "$ipi_driver_patch" || {
			echo "C68_GATE_FAIL: staged IPI provider does not publish virq range" >&2
			exit 1
		}
		grep -Fq 'CPUHP_AP_IRQ_ESP32P4_IPI_STARTING' "$ipi_driver_patch" || {
			echo "C68_GATE_FAIL: staged IPI provider has no CPU-start ordering hook" >&2
			exit 1
		}
		local timer_patch="${patch_stage}/0042-easystick-esp32p4-systimer-cpu1.patch"
		grep -Fq 'CPUHP_AP_IRQ_ESP32P4_TIMER_STARTING' "$timer_patch" || {
			echo "C68_GATE_FAIL: staged SYSTIMER patch has no CPU-start timer hook" >&2
			exit 1
		}
		grep -Fq 'ST_TARGET0_HI + 0x08' "$timer_patch" || {
			echo "C68_GATE_FAIL: staged SYSTIMER patch has no target1 comparator" >&2
			exit 1
		}
		grep -Fq 'writel(1, st_base + st_comp_load[cpu]);' "$timer_patch" || {
			echo "C68_GATE_FAIL: staged SYSTIMER patch has wrong COMPx_LOAD trigger" >&2
			exit 1
		}
		if grep -Fq 'writel(bit, st_base + st_comp_load[cpu]);' "$timer_patch"; then
			echo "C68_GATE_FAIL: staged SYSTIMER patch uses target mask for COMPx_LOAD" >&2
			exit 1
		fi
		local timer_lock_patch="${patch_stage}/0045-easystick-esp32p4-systimer-st-conf-lock.patch"
		if [[ "${EASYSTICK_C68_TIMER_LOCK:-0}" == "1" ]]; then
			grep -Fq 'DEFINE_RAW_SPINLOCK(st_lock)' "$timer_lock_patch" || {
				echo "C68_GATE_FAIL: staged SYSTIMER lock patch has no shared-register lock" >&2
				exit 1
			}
			grep -Fq 'raw_spin_lock_irqsave(&st_lock, flags)' "$timer_lock_patch" || {
				echo "C68_GATE_FAIL: staged SYSTIMER lock patch does not serialize re-arm" >&2
				exit 1
			}
		fi
		local timer_observe_patch="${patch_stage}/0044-easystick-esp32p4-systimer-observe-cpu1.patch"
		if [[ "${EASYSTICK_C68_TIMER_OBSERVE:-0}" == "1" ]]; then
			grep -Fq 'OBS cpu1-set-next' "$timer_observe_patch" || {
				echo "C68_GATE_FAIL: timer observe patch has no CPU1 set_next snapshot" >&2
				exit 1
			}
			grep -Fq 'OBS cpu1-timer-irq' "$timer_observe_patch" || {
				echo "C68_GATE_FAIL: timer observe patch has no CPU1 ISR snapshot" >&2
				exit 1
			}
			grep -Fq 'OBS timer-map' "$timer_observe_patch" || {
				echo "C68_GATE_FAIL: timer observe patch has no INTMTX snapshot" >&2
				exit 1
			}
			grep -Fq 'OBS timer-unmask' "$timer_observe_patch" || {
				echo "C68_GATE_FAIL: timer observe patch has no CPU1 CLIC snapshot" >&2
				exit 1
			}
			grep -Fq 'OBS timer-clic-local' "$timer_observe_patch" || {
				echo "C68_GATE_FAIL: timer observe patch has no CPU1 local CLIC readback" >&2
				exit 1
			}
			grep -Fq 'CLIC_LOCAL_OFF=0x5c' "$timer_observe_patch" || {
				echo "C68_GATE_FAIL: timer observe patch has no CLIC slot-23 offset" >&2
				exit 1
			}
			grep -Fq 'INTMTX1_OFF=0xd8' "$timer_observe_patch" || {
				echo "C68_GATE_FAIL: timer observe patch has no CORE1 INTMTX offset" >&2
				exit 1
			}
			grep -Fq 'OBS cpu1-timer-counts' "$timer_observe_patch" || {
				echo "C68_GATE_FAIL: timer observe patch has no arm/irq/event/rearm ledger" >&2
				exit 1
			}
			grep -Fq 'timer_rearm_count' "$timer_observe_patch" || {
				echo "C68_GATE_FAIL: timer observe patch has no rearm counter" >&2
				exit 1
			}
			grep -Fq 'actual_cpu=%u' "$timer_observe_patch" || {
				echo "C68_GATE_FAIL: timer observe patch has no ISR CPU identity" >&2
				exit 1
			}
			grep -Fq 'OBS cpu1-timer-post-event' "$timer_observe_patch" || {
				echo "C68_GATE_FAIL: timer observe patch has no post-event snapshot" >&2
				exit 1
			}
		fi
		local context_observe_patch="${patch_stage}/0046-easystick-esp32p4-cpu1-context-observe.patch"
		if [[ "${EASYSTICK_C68_CONTEXT_OBSERVE:-0}" == "1" ]]; then
			grep -Fq 'OBS cpu1-context' "$context_observe_patch" || {
				echo "C68_GATE_FAIL: CPU1 context observe patch has no switch trace" >&2
				exit 1
			}
			grep -Fq 'OBS cpu1-illegal' "$context_observe_patch" || {
				echo "C68_GATE_FAIL: CPU1 context observe patch has no illegal-trap trace" >&2
				exit 1
			}
		fi
		local illegal_observe_patch="${patch_stage}/0047-easystick-esp32p4-cpu1-illegal-observe.patch"
		if [[ "${EASYSTICK_C68_ILLEGAL_OBSERVE:-0}" == "1" ]]; then
			grep -Fq 'OBS cpu1-illegal' "$illegal_observe_patch" || {
				echo "C68_GATE_FAIL: CPU1 illegal observe patch has no trap trace" >&2
				exit 1
			}
		fi
		local l3_smoke_patch="${patch_stage}/0048-easystick-esp32p4-l3-smoke-endpoint.patch"
		local l3_workqueue_patch="${patch_stage}/0049-easystick-esp32p4-l3-smoke-workqueue.patch"
		if [[ "${EASYSTICK_C68_L3_SMOKE:-0}" == "1" ]]; then
			grep -Fq '/proc/m5stamp_smp_smoke' "$l3_smoke_patch" || {
				echo "C68_GATE_FAIL: L3 smoke patch has no proc endpoint" >&2
				exit 1
			}
			grep -Fq 'smp_call_function_single' "$l3_smoke_patch" || {
				echo "C68_GATE_FAIL: L3 smoke patch has no cross-call exercise" >&2
				exit 1
			}
			grep -Fq 'schedule_work_on(1' "$l3_workqueue_patch" || {
				echo "C68_GATE_FAIL: L3 reverse call is not deferred from IPI context" >&2
				exit 1
			}
			grep -Fq 'generation++' "$l3_workqueue_patch" || {
				echo "C68_GATE_FAIL: L3 reverse work has no generation advance" >&2
				exit 1
			}
			grep -Fq 'reverse_work_generation' "$l3_workqueue_patch" || {
				echo "C68_GATE_FAIL: L3 reverse work has no generation guard" >&2
				exit 1
			}
			grep -Fq 'work_pending(&result->reverse_work)' "$l3_workqueue_patch" || {
				echo "C68_GATE_FAIL: L3 reverse work has no stale-work assertion" >&2
				exit 1
			}
			grep -Fq 'cancel_work_sync' "$l3_workqueue_patch" || {
				echo "C68_GATE_FAIL: L3 reverse work has no completion cleanup" >&2
				exit 1
			}
		fi
		grep -Fq '<&clic0 23 54 IRQ_TYPE_LEVEL_HIGH>' \
			"${dts_stage}/espressif/easystick-stamp-p4.dts" || {
			echo "C68_GATE_FAIL: staged DTS has no CPU1 SYSTIMER target1 route" >&2
			exit 1
		}
		python3 - "$ipi_driver_patch" \
			"${br_output}/build/linux-custom/arch/riscv/kernel/smpboot.c" <<'PY'
from pathlib import Path
import sys

provider = Path(sys.argv[1]).read_text(encoding="utf-8")
smpboot = Path(sys.argv[2]).read_text(encoding="utf-8")
if provider.rindex("riscv_ipi_set_virq_range(") < provider.index("ret = cpuhp_setup_state"):
    raise SystemExit("C68_GATE_FAIL: virq range is published before CPUHP hook")
if smpboot.index("notify_cpu_starting") > smpboot.index("riscv_ipi_enable"):
    raise SystemExit("C68_GATE_FAIL: CPU1 smpboot enables IPI before CPUHP notification")
PY
		local f
		for f in \
			"${patch_stage}/0038-easystick-c68-clean-release-spinwait.patch" \
			"${patch_stage}/0039-easystick-c68-clean-release-smpboot.patch" \
			"${patch_stage}/0040-easystick-esp32p4-ipi-provider.patch" \
			"${patch_stage}/0041-easystick-esp32p4-ipi-driver.patch" \
			"${patch_stage}/0042-easystick-esp32p4-systimer-cpu1.patch" \
			"${patch_stage}/0049-easystick-esp32p4-l3-smoke-workqueue.patch" \
			"$vmlinux" "$image"; do
			[[ -f "$f" ]] || continue
			if contains_c68dead "$f"; then
				echo "C68_GATE_FAIL: C68DEAD remains in $f" >&2
				exit 1
			fi
		done
		if [[ -f "$timer_observe_patch" ]]; then
			if contains_c68dead "$timer_observe_patch"; then
				echo "C68_GATE_FAIL: C68DEAD remains in timer observe patch" >&2
				exit 1
			fi
		fi
		if [[ -f "$timer_lock_patch" ]]; then
			if contains_c68dead "$timer_lock_patch"; then
				echo "C68_GATE_FAIL: C68DEAD remains in timer lock patch" >&2
				exit 1
			fi
		fi
		if [[ -f "$context_observe_patch" ]]; then
			if contains_c68dead "$context_observe_patch"; then
				echo "C68_GATE_FAIL: C68DEAD remains in CPU1 context observe patch" >&2
				exit 1
			fi
		fi
		if [[ -f "$illegal_observe_patch" ]]; then
			if contains_c68dead "$illegal_observe_patch"; then
				echo "C68_GATE_FAIL: C68DEAD remains in CPU1 illegal observe patch" >&2
				exit 1
			fi
		fi
		if [[ -f "$l3_smoke_patch" ]]; then
			if contains_c68dead "$l3_smoke_patch"; then
				echo "C68_GATE_FAIL: C68DEAD remains in L3 smoke patch" >&2
				exit 1
			fi
		fi
		local manifest_in="${output}/c68-manifest.in.json"
		local timer_lock_manifest=""
		if [[ -f "$timer_lock_patch" ]]; then
			timer_lock_manifest="$timer_lock_patch"
		fi
		local timer_observe_manifest=""
		if [[ -f "$timer_observe_patch" ]]; then
			timer_observe_manifest="$timer_observe_patch"
		fi
		local context_observe_manifest=""
		if [[ -f "$context_observe_patch" ]]; then
			context_observe_manifest="$context_observe_patch"
		fi
		local illegal_observe_manifest=""
		if [[ -f "$illegal_observe_patch" ]]; then
			illegal_observe_manifest="$illegal_observe_patch"
		fi
		local l3_smoke_manifest=""
		if [[ -f "$l3_smoke_patch" ]]; then
			l3_smoke_manifest="$l3_smoke_patch"
		fi
		local tsens_linux_manifest=""
		if [[ "${EASYSTICK_TSENS_LINUX:-0}" == "1" &&
			-f "${patch_stage}/0050-thermal-esp32p4-add-lp-tsens-driver.patch" ]]; then
			tsens_linux_manifest="${patch_stage}/0050-thermal-esp32p4-add-lp-tsens-driver.patch"
		fi
		python3 - "$manifest_in" "$kernel_config" "$image" "$dtb" "$vmlinux" \
			"${EASYSTICK_C68_BOOT_SHIM_BIN:-}" "${EASYSTICK_C68_BOOT_SHIM_ELF:-}" \
			"${patch_stage}/0038-easystick-c68-clean-release-spinwait.patch" \
			"${patch_stage}/0039-easystick-c68-clean-release-smpboot.patch" \
			"${patch_stage}/0040-easystick-esp32p4-ipi-provider.patch" \
			"${patch_stage}/0041-easystick-esp32p4-ipi-driver.patch" \
			"${patch_stage}/0042-easystick-esp32p4-systimer-cpu1.patch" \
			"$timer_lock_manifest" \
			"$context_observe_manifest" \
			"$illegal_observe_manifest" \
			"$timer_observe_manifest" \
			"$l3_smoke_manifest" \
			"$tsens_linux_manifest" \
			"${dts_stage}/espressif/easystick-stamp-p4.dts" \
			"${kernel_config_fragment}" <<'PY'
import json, os, sys
from pathlib import Path
out, kcfg, image, dtb, vmlinux, shim_bin, shim_elf, p38, p39, p40, p41, p42, p45, p46, p47, p44, p48, p50, dts, frag = sys.argv[1:]
files = {
    "kernel_config": kcfg,
    "Image": image,
    "dtb": dtb,
    "vmlinux": vmlinux,
    "patch_0038_staged": p38,
    "patch_0039_staged": p39,
    "patch_0040_staged": p40,
    "patch_0041_staged": p41,
    "patch_0042_staged": p42,
    "staged_dts": dts,
    "kernel_config_fragment": frag,
}
if p45:
    files["patch_0045_staged"] = p45
if p46:
    files["patch_0046_staged"] = p46
if p47:
    files["patch_0047_staged"] = p47
if p44:
    files["patch_0044_staged"] = p44
if p48:
    files["patch_0048_staged"] = p48
if p50:
    files["patch_0050_staged"] = p50
if shim_bin:
    files["boot_shim_bin"] = shim_bin
if shim_elf:
    files["boot_shim_elf"] = shim_elf
payload = {
    "profile": "c68-clean-release",
    "release_pa": os.environ.get("EASYSTICK_C68_RELEASE_PA", ""),
    "stage_pa": os.environ.get("EASYSTICK_C68_STAGE_PA", ""),
    "entry_pa": os.environ.get("EASYSTICK_C68_ENTRY_PA", ""),
    "files": files,
    "kernel_config_digest_note": "sha256 recorded by write_manifest.py",
}
Path(out).write_text(json.dumps(payload), encoding="utf-8")
PY
		python3 "${firmware_root}/linux/c68/write_manifest.py" \
			"${output}/c68-manifest.json" "$manifest_in"
		echo "C68 fail-closed gates passed: ${output}/c68-manifest.json"
	else
		if grep -Fqx -- "CONFIG_SMP=y" "$kernel_config"; then
			echo "UP kernel config gate failed: CONFIG_SMP=y without C68 profile" >&2
			exit 1
		fi
	fi
}

profile_fail_closed_after_rootfs() {
	local config="${br_output}/.config"
	local target="${br_output}/target"

	if $network_profile; then
		grep -Fqx -- "BR2_PACKAGE_ESP_HOSTED_NG=y" "$config" || {
			echo "profile gate failed: network profile has no ESP-Hosted package" >&2
			exit 1
		}
	else
		for expected in \
			"# BR2_PACKAGE_ESP_HOSTED_NG is not set" \
			"# BR2_PACKAGE_IW is not set"; do
			grep -Fqx -- "$expected" "$config" || {
				echo "profile gate failed: non-network profile has ${expected}" >&2
				exit 1
			}
		done
		for stale in \
			"${target}/etc/init.d/S01seedrng" \
			"${target}/etc/init.d/S40network" \
			"${target}/etc/init.d/S41timesync" \
			"${target}/usr/sbin/easystick-sdio-diag"; do
			[[ ! -e "$stale" ]] || {
				echo "profile gate failed: non-network rootfs contains ${stale}" >&2
				exit 1
			}
		done
	fi

	if $ssh_profile; then
		grep -Fqx -- "BR2_PACKAGE_DROPBEAR=y" "$config" || {
			echo "profile gate failed: SSH profile has no Dropbear config" >&2
			exit 1
		}
	else
		expected="# BR2_PACKAGE_DROPBEAR is not set"
		grep -Fqx -- "$expected" "$config" || {
			echo "profile gate failed: non-SSH profile has ${expected}" >&2
			exit 1
		}
		for stale in \
			"${target}/etc/init.d/S50dropbear" \
			"${target}/etc/init.d/S85easystick-ssh" \
			"${target}/usr/sbin/dropbear" \
			"${target}/usr/bin/dropbear" \
			"${target}/etc/dropbear"; do
			[[ ! -e "$stale" ]] || {
				echo "profile gate failed: non-SSH rootfs contains ${stale}" >&2
				exit 1
			}
		done
	fi

	if [[ "${EASYSTICK_C68_L3_SMOKE:-0}" == "1" ]]; then
		grep -Fqx -- "BR2_PACKAGE_M5STAMP_SMP_SMOKE=y" "$config" || {
			echo "profile gate failed: L3 profile has no M5Stamp smoke package" >&2
			exit 1
		}
	else
		grep -Fqx -- "# BR2_PACKAGE_M5STAMP_SMP_SMOKE is not set" "$config" || {
			echo "profile gate failed: non-L3 profile has M5Stamp smoke package" >&2
			exit 1
		}
		[[ ! -e "${target}/usr/sbin/m5stamp-smp-smoke" ]] || {
			echo "profile gate failed: non-L3 rootfs contains M5Stamp smoke helper" >&2
			exit 1
		}
	fi
	if $c68_stress_profile; then
		grep -Fqx -- "BR2_PACKAGE_M5STAMP_SMP_STRESS=y" "$config" || {
			echo "profile gate failed: stress profile has no M5Stamp stress package" >&2
			exit 1
		}
	else
		grep -Fqx -- "# BR2_PACKAGE_M5STAMP_SMP_STRESS is not set" "$config" || {
			echo "profile gate failed: non-stress profile has M5Stamp stress package" >&2
			exit 1
		}
		[[ ! -e "${target}/usr/sbin/m5stamp-smp-stress" ]] || {
			echo "profile gate failed: non-stress rootfs contains M5Stamp stress helper" >&2
			exit 1
		}
	fi
	if [[ "$stacktrace_diagnostics" == "1" ]]; then
		local rootfs_image="${br_output}/images/rootfs.squashfs"
		[[ -s "$rootfs_image" ]] || {
			echo "stacktrace diagnostic gate failed: rootfs image missing" >&2
			exit 1
		}
		local rootfs_bytes
		rootfs_bytes=$(stat -c '%s' "$rootfs_image")
		# physmap-core rounds the 7 MiB mtd-rom resource down to a 4 MiB
		# map when no address GPIO window is present.  A larger SquashFS
		# cannot be mounted even though it fits the flash partition.
		(( rootfs_bytes <= 0x400000 )) || {
			echo "stacktrace diagnostic gate failed: rootfs=${rootfs_bytes} exceeds 4 MiB physmap window" >&2
			exit 1
		}
		echo "Stacktrace diagnostic rootfs window gate passed: ${rootfs_bytes} bytes"
	fi
}

if $print_only; then
	echo "${profile^^} preflight passed; would run:"
	printf '  make'; printf ' %q' "${make_args[@]}"; echo " easystick_stamp_p4_defconfig"
	printf '  make'; printf ' %q' "${make_args[@]}"; echo " olddefconfig"
	printf '  make'; printf ' %q' "${make_args[@]}"; echo " all"
	exit 0
fi

# The locked Linux checkout is intentionally shallow, so use a deterministic
# source archive rather than asking Buildroot's Git backend to traverse a
# missing parent history or to download the same kernel again.  The archive is
# generated outside Git and is reused by subsequent incremental builds.
if [[ ! -s "$linux_tarball" ]]; then
	linux_epoch=$(git -C "$linux_root" log -1 --format=%ct)
	linux_archive_dir="linux-${linux_commit}"
	linux_tarball_tmp="${linux_tarball}.tmp.$$"
	tar --sort=name --mtime="@${linux_epoch}" --owner=0 --group=0 --numeric-owner \
		--mode='go=u,go-w' --exclude='./.git' \
		--transform="s#^\./#${linux_archive_dir}/#" \
		-C "$linux_root" -czf "$linux_tarball_tmp" .
	mv -f -- "$linux_tarball_tmp" "$linux_tarball"
fi

if [[ "${EASYSTICK_CMD53_BB_SKIP_BR_CLEAN:-0}" == "1" ]]; then
	echo "CMD53_BB: skipping Buildroot clean (EASYSTICK_CMD53_BB_SKIP_BR_CLEAN=1); linux-dirclean still applies" >&2
	# Keep stamp in sync so a later non-skip rebuild still sees the real identity.
	mkdir -p -- "$(dirname -- "$profile_stamp")"
	printf '%s\n' "$profile_identity" >"$profile_stamp"
elif [[ -d "$br_output" && ! -f "$profile_stamp" &&
      ( -f "${br_output}/.config" ||
        -d "${br_output}/target" ||
        -d "${br_output}/build" ) ]]; then
	echo "Buildroot output has no profile stamp; cleaning ${br_output}" >&2
	make "${make_args[@]}" clean
fi
if [[ "${EASYSTICK_CMD53_BB_SKIP_BR_CLEAN:-0}" != "1" && -f "$profile_stamp" ]]; then
	previous_profile=""
	previous_profile=$(<"$profile_stamp")
	if [[ "$previous_profile" != "$profile_identity" ]]; then
		echo "Buildroot profile changed; cleaning ${br_output}" >&2
		make "${make_args[@]}" clean
		rm -f -- "$profile_stamp"
	fi
fi

make "${make_args[@]}" easystick_stamp_p4_defconfig
# Buildroot's defconfig intentionally contains the board defaults only.  Lock
# the source revision and patch staging path in the generated output so the
# repository does not duplicate a moving hash or vendor reference patches.
set_config_value "${br_output}/.config" BR2_LINUX_KERNEL_CUSTOM_VERSION "# BR2_LINUX_KERNEL_CUSTOM_VERSION is not set"
set_config_value "${br_output}/.config" BR2_LINUX_KERNEL_CUSTOM_GIT "# BR2_LINUX_KERNEL_CUSTOM_GIT is not set"
set_config_value "${br_output}/.config" BR2_LINUX_KERNEL_CUSTOM_TARBALL y
set_config_value "${br_output}/.config" BR2_LINUX_KERNEL_CUSTOM_TARBALL_LOCATION "\"file://${linux_tarball}\""
set_config_value "${br_output}/.config" BR2_LINUX_KERNEL_PATCH "\"${patch_stage}\""
set_config_value "${br_output}/.config" BR2_LINUX_KERNEL_USE_CUSTOM_CONFIG y
set_config_value "${br_output}/.config" BR2_LINUX_KERNEL_CUSTOM_CONFIG_FILE "\"${firmware_root}/linux/linux.config\""
set_config_value "${br_output}/.config" BR2_LINUX_KERNEL_DTS_SUPPORT y
set_config_value "${br_output}/.config" BR2_LINUX_KERNEL_INTREE_DTS_NAME '\"espressif/easystick-stamp-p4\"'
set_config_value "${br_output}/.config" BR2_LINUX_KERNEL_CUSTOM_DTS_PATH "# BR2_LINUX_KERNEL_CUSTOM_DTS_PATH is not set"
set_config_value "${br_output}/.config" BR2_LINUX_KERNEL_CUSTOM_DTS_DIR "\"${dts_stage}\""
set_config_value "${br_output}/.config" BR2_RISCV_USE_MMU "# BR2_RISCV_USE_MMU is not set"
set_config_value "${br_output}/.config" BR2_BINFMT_FLAT y
if $network_profile; then
	set_config_value "${br_output}/.config" BR2_PACKAGE_EASYSTICK_STAMP_P4_M2 y
	set_config_value "${br_output}/.config" BR2_PACKAGE_EASYSTICK_STAMP_P4_M1 "# BR2_PACKAGE_EASYSTICK_STAMP_P4_M1 is not set"
	set_config_value "${br_output}/.config" BR2_PACKAGE_ESP_HOSTED_NG y
	set_config_value "${br_output}/.config" BR2_PACKAGE_IW y
	set_config_value "${br_output}/.config" BR2_GLOBAL_PATCH_DIR "\"${global_patch_stage}\""
else
	set_config_value "${br_output}/.config" BR2_PACKAGE_EASYSTICK_STAMP_P4_M2 "# BR2_PACKAGE_EASYSTICK_STAMP_P4_M2 is not set"
	set_config_value "${br_output}/.config" BR2_PACKAGE_EASYSTICK_STAMP_P4_M1 y
	set_config_value "${br_output}/.config" BR2_PACKAGE_ESP_HOSTED_NG "# BR2_PACKAGE_ESP_HOSTED_NG is not set"
	set_config_value "${br_output}/.config" BR2_PACKAGE_IW "# BR2_PACKAGE_IW is not set"
	set_config_value "${br_output}/.config" BR2_GLOBAL_PATCH_DIR "# BR2_GLOBAL_PATCH_DIR is not set"
fi
if [[ "${EASYSTICK_C68_L3_SMOKE:-0}" == "1" ]]; then
	[[ "$c68_profile" == true ]] || {
		echo "L3 smoke requires EASYSTICK_C68_CLEAN_RELEASE=1" >&2
		exit 1
	}
	[[ "${EASYSTICK_LINUX_ONLY:-0}" != "1" ]] || {
		echo "L3 smoke requires a full Buildroot build, not EASYSTICK_LINUX_ONLY=1" >&2
		exit 1
	}
	set_config_value "${br_output}/.config" BR2_PACKAGE_M5STAMP_SMP_SMOKE y
fi
if $c68_stress_profile; then
	[[ "$c68_profile" == true ]] || {
		echo "C68 stress requires EASYSTICK_C68_CLEAN_RELEASE=1" >&2
		exit 1
	}
	[[ "${EASYSTICK_C68_L3_SMOKE:-0}" == "1" ]] || {
		echo "C68 stress requires EASYSTICK_C68_L3_SMOKE=1" >&2
		exit 1
	}
	[[ "${EASYSTICK_LINUX_ONLY:-0}" != "1" ]] || {
		echo "C68 stress requires a full Buildroot build" >&2
		exit 1
	}
	set_config_value "${br_output}/.config" BR2_PACKAGE_M5STAMP_SMP_STRESS y
else
	set_config_value "${br_output}/.config" BR2_PACKAGE_M5STAMP_SMP_STRESS "# BR2_PACKAGE_M5STAMP_SMP_STRESS is not set"
fi
if [[ "${EASYSTICK_C68_L3_SMOKE:-0}" != "1" ]]; then
	set_config_value "${br_output}/.config" BR2_PACKAGE_M5STAMP_SMP_SMOKE "# BR2_PACKAGE_M5STAMP_SMP_SMOKE is not set"
fi

rootfs_overlay="${external_root}/board/easystick-stamp-p4/rootfs-overlay"
post_build_scripts="${external_root}/board/easystick-stamp-p4/post-build.sh"
users_table="${external_root}/board/easystick-stamp-p4/users.txt"
busybox_fragments=""
if $network_profile; then
	# M2 loads esp32_sdio.ko from S40network.  Keep the module-loader
	# applets enabled even when no SSH profile is selected; otherwise the
	# script's deliberately swallowed `modprobe` failure leaves SDIO
	# discovery looking healthy while wlan0 can never appear.
	busybox_fragments="${external_root}/board/easystick-stamp-p4/busybox.fragment"
fi
if $ssh_profile; then
	rootfs_overlay="${rootfs_overlay} ${m3_stage}/rootfs-overlay"
	post_build_scripts="${post_build_scripts} ${m3_stage}/post-build.sh"
	users_table="${m3_stage}/users.txt"
	busybox_fragments="${m3_stage}/busybox.fragment"
	localoptions="${m3_stage}/dropbear-localoptions.h"
	if $lab_profile; then
		rootfs_overlay="${rootfs_overlay} ${lab_stage}/rootfs-overlay"
		rootfs_overlay="${rootfs_overlay} ${lab_provision_stage}"
		post_build_scripts="${post_build_scripts} ${lab_stage}/post-build.sh"
		users_table="${lab_stage}/users.txt"
		busybox_fragments="${busybox_fragments} ${lab_stage}/busybox.fragment"
		localoptions="${lab_stage}/dropbear-localoptions.h"
	fi
	set_config_value "${br_output}/.config" BR2_ROOTFS_OVERLAY "\"${rootfs_overlay}\""
	set_config_value "${br_output}/.config" BR2_ROOTFS_POST_BUILD_SCRIPT "\"${post_build_scripts}\""
	set_config_value "${br_output}/.config" BR2_ROOTFS_USERS_TABLES "\"${users_table}\""
	set_config_value "${br_output}/.config" BR2_PACKAGE_DROPBEAR y
	if $lab_profile; then
		# Lab needs dbclient for loopback SSH proof and tcpdump for
		# directional ARP/ICMP/TCP/22 evidence.  Production m3 stays
		# client-less.
		set_config_value "${br_output}/.config" BR2_PACKAGE_DROPBEAR_CLIENT y
		set_config_value "${br_output}/.config" BR2_PACKAGE_LIBPCAP y
		set_config_value "${br_output}/.config" BR2_PACKAGE_TCPDUMP y
	else
		set_config_value "${br_output}/.config" BR2_PACKAGE_DROPBEAR_CLIENT "# BR2_PACKAGE_DROPBEAR_CLIENT is not set"
	fi
	set_config_value "${br_output}/.config" BR2_PACKAGE_DROPBEAR_DISABLE_REVERSEDNS y
	set_config_value "${br_output}/.config" BR2_PACKAGE_DROPBEAR_LOCALOPTIONS_FILE "\"${localoptions}\""
else
	set_config_value "${br_output}/.config" BR2_PACKAGE_DROPBEAR "# BR2_PACKAGE_DROPBEAR is not set"
	set_config_value "${br_output}/.config" BR2_PACKAGE_DROPBEAR_CLIENT "# BR2_PACKAGE_DROPBEAR_CLIENT is not set"
	set_config_value "${br_output}/.config" BR2_PACKAGE_DROPBEAR_SMALL "# BR2_PACKAGE_DROPBEAR_SMALL is not set"
	set_config_value "${br_output}/.config" BR2_PACKAGE_DROPBEAR_DISABLE_REVERSEDNS "# BR2_PACKAGE_DROPBEAR_DISABLE_REVERSEDNS is not set"
	set_config_value "${br_output}/.config" BR2_PACKAGE_DROPBEAR_LOCALOPTIONS_FILE "# BR2_PACKAGE_DROPBEAR_LOCALOPTIONS_FILE is not set"
fi
if $lab_profile; then
	set_config_value "${br_output}/.config" BR2_PACKAGE_LIBPCAP y
	set_config_value "${br_output}/.config" BR2_PACKAGE_TCPDUMP y
	set_config_value "${br_output}/.config" BR2_PACKAGE_MICROPYTHON_NOMMU y
else
	set_config_value "${br_output}/.config" BR2_PACKAGE_LIBPCAP "# BR2_PACKAGE_LIBPCAP is not set"
	set_config_value "${br_output}/.config" BR2_PACKAGE_TCPDUMP "# BR2_PACKAGE_TCPDUMP is not set"
	set_config_value "${br_output}/.config" BR2_PACKAGE_MICROPYTHON_NOMMU "# BR2_PACKAGE_MICROPYTHON_NOMMU is not set"
fi
set_config_value "${br_output}/.config" BR2_ROOTFS_OVERLAY "\"${rootfs_overlay}\""
set_config_value "${br_output}/.config" BR2_ROOTFS_POST_BUILD_SCRIPT "\"${post_build_scripts}\""
set_config_value "${br_output}/.config" BR2_ROOTFS_USERS_TABLES "\"${users_table}\""
if $c68_top_profile; then
	if [[ -n "${busybox_fragments:-}" ]]; then
		busybox_fragments="${busybox_fragments} ${c68_profile_stage}/busybox.fragment"
	else
		busybox_fragments="${c68_profile_stage}/busybox.fragment"
	fi
fi
if [[ -n "${busybox_fragments:-}" ]]; then
	set_config_value "${br_output}/.config" \
		BR2_PACKAGE_BUSYBOX_CONFIG_FRAGMENT_FILES "\"${busybox_fragments}\""
else
	set_config_value "${br_output}/.config" \
		BR2_PACKAGE_BUSYBOX_CONFIG_FRAGMENT_FILES \
		"# BR2_PACKAGE_BUSYBOX_CONFIG_FRAGMENT_FILES is not set"
fi
if $network_profile; then
	set_config_value "${br_output}/.config" BR2_PACKAGE_WPA_SUPPLICANT_AP_SUPPORT y
	set_config_value "${br_output}/.config" BR2_PACKAGE_WPA_SUPPLICANT_CTRL_IFACE y
fi
make "${make_args[@]}" olddefconfig
printf '%s\n' "$profile_identity" >"$profile_stamp"
if ! $c68_top_profile &&
	grep -Fq "c68-profile/busybox.fragment" "${br_output}/.config"; then
	echo "UP profile gate failed: C68 BusyBox fragment leaked into UP config" >&2
	exit 1
fi
if $network_profile; then
	# ESP-Hosted is supplied through a local, version-locked source tree and
	# receives board patches from BR2_EXTERNAL.  Buildroot does not track a
	# changed patch file as a package dependency, so discard only this package
	# before each network-profile build.  The kernel, toolchain, download cache,
	# and compiler cache remain intact while ensuring the tested module matches
	# the checked-in patch series.
	make "${make_args[@]}" esp-hosted-ng-dirclean
	make "${make_args[@]}" wpa_supplicant-dirclean
	make "${make_args[@]}" busybox-dirclean
fi
if $ssh_profile; then
	# Dropbear's localoptions header is appended during its extract hook.  A
	# reused Buildroot output otherwise keeps the previous profile's generated
	# header even when BR2_PACKAGE_DROPBEAR_LOCALOPTIONS_FILE changes.
	make "${make_args[@]}" dropbear-dirclean
fi
if [[ "${EASYSTICK_C68_L3_SMOKE:-0}" == "1" ]]; then
	# The L3 endpoint is a kernel patch, so a reused linux-custom tree can
	# otherwise retain the previous 0045+0047 image without applying 0048.
	make "${make_args[@]}" linux-dirclean
fi
if [[ "${EASYSTICK_CMD53_RETENTION_BB:-0}" == "1" ]]; then
	# nm PA is baked into rendered 0052. A reused linux-custom tree can keep
	# a previous PA even when profile_identity also lists the new one, so
	# retention BB always discards the kernel build directory.
	make "${make_args[@]}" linux-dirclean
fi
if [[ "${EASYSTICK_LINUX_ONLY:-0}" == "1" ]]; then
	make "${make_args[@]}" linux-dirclean
	make "${make_args[@]}" linux
	wdt_crash_fail_closed_after_linux
	wdt_inject_fail_closed_after_linux
	c68_fail_closed_after_linux
	echo "Linux-only rebuild complete: ${br_output}/images/Image"
	exit 0
fi
make "${make_args[@]}" all
wdt_crash_fail_closed_after_linux
wdt_inject_fail_closed_after_linux

# Catch a stale Buildroot host toolchain before any image is considered
# flashable.  The P4 HP core is RV32IMAC; a previous output directory can keep
# an older GCC configured for the default G/IMAFD profile even after
# olddefconfig selects the custom ISA.  Rebuilding must therefore fail closed
# rather than producing a userland with illegal F/D instructions.
target_cc="${br_output}/host/bin/riscv32-buildroot-linux-uclibc-gcc"
[[ -x "$target_cc" ]] || {
	echo "toolchain check failed: missing $target_cc" >&2
	exit 1
}
target_flags=$("$target_cc" -Q --help=target 2>/dev/null)
target_abi=$(awk '$1 == "-mabi=" { print $2 }' <<<"$target_flags")
target_march=$(awk '$1 == "-march=" { print $2 }' <<<"$target_flags")
[[ "$target_abi" == ilp32 ]] || {
	echo "toolchain check failed: GCC default ABI is not ilp32" >&2
	"$target_cc" -Q --help=target | grep -E 'mabi|march' >&2 || true
	exit 1
}
[[ "$target_march" == rv32imac* ]] || {
	echo "toolchain check failed: GCC default ISA is not rv32imac" >&2
	"$target_cc" -Q --help=target | grep -E 'mabi|march' >&2 || true
	exit 1
}

kernel_config="${br_output}/build/linux-custom/.config"
for expected in \
	"CONFIG_32BIT=y" \
	"# CONFIG_MMU is not set" \
	"CONFIG_BINFMT_FLAT=y"; do
	grep -Fqx -- "${expected}" "${kernel_config}" || {
		echo "kernel config check failed: ${expected}" >&2
		exit 1
	}
done
if $network_profile; then
	for expected in \
		"CONFIG_MMC=y" \
		"CONFIG_MMC_DW=y" \
		"CONFIG_MODULES=y" \
		"CONFIG_WLAN=y" \
		"CONFIG_CFG80211=y" \
		"CONFIG_SERIAL_ESP32_ACM=y"; do
		grep -Fqx -- "${expected}" "${kernel_config}" || {
			echo "M2 kernel config check failed: ${expected}" >&2
			exit 1
		}
	done
fi
if [[ "$stacktrace_diagnostics" == "1" ]]; then
	for expected in \
		"CONFIG_STACKTRACE=y" \
		"CONFIG_KALLSYMS=y" \
		"CONFIG_KALLSYMS_ALL=y" \
		"CONFIG_DEBUG_INFO=y" \
		"CONFIG_DEBUG_INFO_DWARF4=y" \
		"CONFIG_FRAME_POINTER=y"; do
		grep -Fqx -- "${expected}" "${kernel_config}" || {
			echo "stacktrace diagnostic config check failed: ${expected}" >&2
			exit 1
		}
	done
fi
if $ssh_profile; then
	grep -Fqx -- "BR2_PACKAGE_DROPBEAR=y" "${br_output}/.config" || {
		echo "SSH profile config check failed: BR2_PACKAGE_DROPBEAR=y" >&2
		exit 1
	}
	if $lab_profile; then
		for expected in \
			"BR2_PACKAGE_DROPBEAR_CLIENT=y" \
			"BR2_PACKAGE_LIBPCAP=y" \
			"BR2_PACKAGE_TCPDUMP=y"; do
			grep -Fqx -- "${expected}" "${br_output}/.config" || {
				echo "m3-lab config check failed: ${expected}" >&2
				exit 1
			}
		done
		[[ -x "${br_output}/target/usr/bin/dbclient" || \
		   -x "${br_output}/target/usr/sbin/dbclient" ]] || {
			echo "m3-lab check failed: Dropbear client missing" >&2
			exit 1
		}
		[[ -x "${br_output}/target/usr/bin/tcpdump" || \
		   -x "${br_output}/target/usr/sbin/tcpdump" ]] || {
			echo "m3-lab check failed: tcpdump missing" >&2
			exit 1
		}
	else
		grep -Fqx -- "# BR2_PACKAGE_DROPBEAR_CLIENT is not set" "${br_output}/.config" || {
			echo "SSH profile config check failed: Dropbear client must stay disabled" >&2
			exit 1
		}
	fi
	[[ -x "${br_output}/target/usr/sbin/inetd" ]] || {
		echo "SSH profile check failed: BusyBox inetd missing" >&2
		exit 1
	}
	[[ -x "${br_output}/target/usr/sbin/dropbear" ]] || {
		echo "SSH profile check failed: Dropbear missing" >&2
		exit 1
	}
fi
if grep -Eq '^CONFIG_64BIT=' "${kernel_config}"; then
	echo "kernel config check failed: CONFIG_64BIT must be disabled" >&2
	exit 1
fi

c68_fail_closed_after_linux

if $c68_top_profile; then
	[[ -x "${br_output}/target/bin/top" ||
	   -x "${br_output}/target/usr/bin/top" ]] || {
		echo "C68_GATE_FAIL: BusyBox top is missing from the target rootfs" >&2
		exit 1
	}
	for top_option in \
		CONFIG_TOP=y \
		CONFIG_FEATURE_TOP_CPU_USAGE_PERCENTAGE=y \
		CONFIG_FEATURE_TOP_CPU_GLOBAL_PERCENTS=y \
		CONFIG_FEATURE_TOP_SMP_CPU=y \
		CONFIG_FEATURE_TOP_SMP_PROCESS=y \
		CONFIG_FEATURE_TOPMEM=y; do
		python3 - "${br_output}/build" "$top_option" <<'PY'
from pathlib import Path
import sys

build_root, expected = sys.argv[1:]
configs = sorted(Path(build_root).glob("busybox-*/.config"))
if not any(expected in path.read_text(encoding="utf-8", errors="replace").splitlines()
           for path in configs):
    raise SystemExit(
        f"C68_GATE_FAIL: BusyBox config is missing {expected}"
    )
PY
	done
fi

profile_fail_closed_after_rootfs
printf '%s\n' "$profile_identity" >"$profile_stamp"
echo "${profile^^} Buildroot build complete: ${br_output}/images"
