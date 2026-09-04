#!/usr/bin/env bash
# C68 fail-closed profile: boot-shim first, nm-render 0038/0039, stage A'
# and CPU1 SYSTIMER delivery,
# then build-m1.
# Never writes flash. Requires IDF_PATH / idf.py (same as boot-shim/build.sh).
set -euo pipefail

usage() {
	cat >&2 <<'EOF'
Usage: build-c68.sh <external-source-root|--vendor> <build-output> [--profile m1|m2|m3|m3-lab] [--print-only]

Builds the C68 boot-shim, extracts c68_release / c68_stage / c68_secondary_entry,
renders 0038/0039 into <build-output>/c68-rendered-patches, stages 0040/0041/0042,
then invokes build-m1.sh with EASYSTICK_C68_CLEAN_RELEASE=1.

Set EASYSTICK_C68_L3_SMOKE=1 to include the m5stamp smoke helper and the
C68-only BusyBox top fragment.  Add EASYSTICK_C68_STRESS=1 for the bounded
m5stamp process/SMP stress helper; stress requires the L3 endpoint and a full
Buildroot build.  Set EASYSTICK_TSENS_ORACLE=1 for an explicit C68 boot-shim
LP-TSENS reference capture; that build is diagnostic and must not be confused
with the normal SMP baseline.  Set EASYSTICK_TSENS_LINUX=1 separately to
include the experimental Linux LP-TSENS driver; it programs and verifies the
REGI2C DAC from Linux and exposes a tripless /sys/class/thermal zone.
EOF
}

if [[ $# -lt 2 ]]; then
	usage
	exit 2
fi

source_arg=$1
output=$(mkdir -p -- "$2" && cd -- "$2" && pwd)
shift 2
extra_args=("$@")

script_dir=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)
firmware_root=$(cd -- "${script_dir}/.." && pwd)
shim_src="${script_dir}/boot-shim"
patch_src="${script_dir}/kernel-patches"
c68_dir="${script_dir}/c68"
rendered="${output}/c68-rendered-patches"
shim_out="${output}/boot-shim-c68"
elf="${shim_out}/build/easystick_stamp_p4_boot_shim.elf"

command -v idf.py >/dev/null || {
	echo "C68_GATE_FAIL: idf.py is not on PATH; source ESP-IDF export.sh first" >&2
	exit 1
}
command -v riscv32-esp-elf-nm >/dev/null || {
	echo "C68_GATE_FAIL: riscv32-esp-elf-nm is not on PATH" >&2
	exit 1
}

export EASYSTICK_C68_CLEAN_RELEASE=1
"${shim_src}/build.sh" "$shim_out" --profile c68
[[ -f "$elf" ]] || {
	echo "C68_GATE_FAIL: missing $elf" >&2
	exit 1
}
if [[ "${EASYSTICK_TSENS_ORACLE:-0}" == "1" ]]; then
	grep -aFq 'P4_TSENS_REF PASS' "$elf" || {
		echo "C68_GATE_FAIL: TSENS oracle was requested but not linked" >&2
		exit 1
	}
	echo "C68_TSENS_ORACLE enabled: runtime P4_TSENS_REF capture required"
fi

grep -q '#define CONFIG_ESP_SYSTEM_SINGLE_CORE_MODE 1' \
	"${shim_out}/build/config/sdkconfig.h" || {
	echo "C68_GATE_FAIL: CONFIG_ESP_SYSTEM_SINGLE_CORE_MODE is not 1" >&2
	exit 1
}

nm_out=$(riscv32-esp-elf-nm "$elf")
nm_release=$(awk '/ c68_release$/{print $1; exit}' <<<"$nm_out")
nm_stage=$(awk '/ c68_stage$/{print $1; exit}' <<<"$nm_out")
nm_entry=$(awk '/ c68_secondary_entry$/{print $1; exit}' <<<"$nm_out")
[[ -n "$nm_release" && -n "$nm_stage" && -n "$nm_entry" ]] || {
	echo "C68_GATE_FAIL: nm missing c68_release/c68_stage/c68_secondary_entry" >&2
	riscv32-esp-elf-nm "$elf" | grep c68 || true
	exit 1
}
release_pa="0x${nm_release}u"
stage_pa="0x${nm_stage}u"
entry_pa="0x${nm_entry}"
echo "C68_NM release=$release_pa stage=$stage_pa entry=$entry_pa"

rm -rf -- "$rendered"
python3 "${c68_dir}/render_patches.py" "$patch_src" "$rendered" "$release_pa" "$stage_pa"

printf '%s\n' "$release_pa" >"${output}/c68-release.pa"
printf '%s\n' "$stage_pa" >"${output}/c68-stage.pa"
printf '%s\n' "$entry_pa" >"${output}/c68-entry.pa"

export EASYSTICK_KERNEL_PATCH_OVERRIDE_DIR="$rendered"
export EASYSTICK_C68_RELEASE_PA="$release_pa"
export EASYSTICK_C68_STAGE_PA="$stage_pa"
export EASYSTICK_C68_ENTRY_PA="$entry_pa"
export EASYSTICK_C68_BOOT_SHIM_BIN="${shim_out}/build/easystick_stamp_p4_boot_shim.bin"
export EASYSTICK_C68_BOOT_SHIM_ELF="$elf"

exec "${script_dir}/build-m1.sh" "$source_arg" "$output" "${extra_args[@]}"
