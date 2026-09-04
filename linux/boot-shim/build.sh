#!/usr/bin/env bash
set -euo pipefail

usage() {
	cat >&2 <<'EOF'
Usage: build.sh <output-directory> [--profile m1|m2|c68]

Requires IDF_PATH to point at the locked ESP-IDF checkout and idf.py to be
available in that environment. The script only builds the ESP-IDF shim; it
never invokes esptool or writes a target. Use a separate output directory so
the M1 recovery image cannot be overwritten by an M2 experiment.

Set EASYSTICK_TSENS_ORACLE=1 for an explicit C68-only ESP32-P4 LP-TSENS
reference build. It emits one P4_TSENS_REF PASS line, then disables the
sensor and its internal REGI2C path before handing off to Linux.
EOF
}

[[ $# -ge 1 && $# -le 3 ]] || { usage; exit 2; }
output=$(mkdir -p -- "$1" && cd -- "$1" && pwd)
profile=m1
shift
while [[ $# -gt 0 ]]; do
	case "$1" in
		--profile)
			[[ $# -ge 2 ]] || { usage; exit 2; }
			profile=$2
			shift 2
			;;
		*) usage; exit 2 ;;
	esac
done
case "$profile" in m1|m2|c68) ;; *) echo "unsupported profile: $profile" >&2; exit 2 ;; esac
if [[ "${EASYSTICK_TSENS_ORACLE:-0}" == "1" && "$profile" != "c68" ]]; then
	echo "EASYSTICK_TSENS_ORACLE requires the C68 single-core profile" >&2
	exit 2
fi

script_dir=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)
project_dir=$(cd -- "${script_dir}" && pwd)
[[ -n "${IDF_PATH:-}" && -f "${IDF_PATH}/tools/idf.py" ]] || {
	echo "IDF_PATH must point to the locked ESP-IDF checkout" >&2
	exit 1
}
command -v idf.py >/dev/null || {
	echo "idf.py is not on PATH; source ESP-IDF export.sh first" >&2
	exit 1
}

export IDF_TARGET=esp32p4
export EASYSTICK_BOOT_PROFILE="$profile"
if [[ "$profile" == c68 ]]; then
	export EASYSTICK_C68_CLEAN_RELEASE=1
	# The checked-in sdkconfig is a dual-core M1/M2 snapshot
	# (# CONFIG_FREERTOS_UNICORE is not set). C68 must not inherit it.
	c68_src="${output}/boot-shim-src"
	rm -rf -- "$c68_src"
	mkdir -p -- "$c68_src"
	tar -C "$project_dir" --exclude sdkconfig --exclude sdkconfig.old \
		--exclude build -cf - . | tar -C "$c68_src" -xf -
	project_dir=$c68_src
fi
export SDKCONFIG="${output}/sdkconfig"
if [[ "$profile" == m2 || "$profile" == c68 ]]; then
	export SDKCONFIG_DEFAULTS="${project_dir}/sdkconfig.defaults;${project_dir}/sdkconfig-m2.defaults"
else
	export SDKCONFIG_DEFAULTS="${project_dir}/sdkconfig.defaults"
fi

rm -f "${output}/sdkconfig" "${output}/sdkconfig.old"
if [[ "$profile" == c68 ]]; then
	rm -rf -- "${output}/build"
fi

idf.py -B "${output}/build" -C "$project_dir" reconfigure build

for artifact in bootloader/bootloader.bin partition_table/partition-table.bin easystick_stamp_p4_boot_shim.bin; do
	[[ -s "${output}/build/${artifact}" ]] || {
		echo "missing ESP-IDF artifact: ${output}/build/${artifact}" >&2
		exit 1
	}
done
echo "${profile^^} boot-shim build complete: ${output}/build"
