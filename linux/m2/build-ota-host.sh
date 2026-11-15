#!/usr/bin/env bash
set -euo pipefail

usage() {
	cat >&2 <<'EOF'
Usage: build-ota-host.sh <external-source-root> <c6-network-adapter.bin> <output-directory>

Builds a disposable ESP-IDF ESP32-P4 host that uses ESP-Hosted-MCU over the
C6 SDIO link to update the C6 firmware partition. It never flashes the P4 or
C6. The ESP-Hosted-MCU source is copied from the external cache
(<external-source-root>/esp_hosted_mcu, v2.13.0 or newer); no third-party
source is vendored into this repository.

ES_BOARD selects the wiring. The default, stamp-p4, is the Stamp-AddOn C6.
ES_BOARD=tab5 uses ota-host/sdkconfig-tab5.defaults (the Tab5 SDIO pins, C6
reset on GPIO15) and adds the tab5_c6_power component, which switches the C6
supply on through the IO expander before ESP-Hosted resets it.
EOF
}

[[ $# -eq 3 ]] || { usage; exit 2; }
source_root=$(cd -- "$1" && pwd)
c6_image=$(cd -- "$(dirname -- "$2")" && pwd)/$(basename -- "$2")
output=$(mkdir -p -- "$3" && cd -- "$3" && pwd)
script_dir=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)
idf_root="${IDF_PATH:-${source_root}/esp_idf}"
host_source="${source_root}/esp_hosted_mcu"

[[ -f "${idf_root}/tools/idf.py" ]] || { echo "ESP-IDF is missing: ${idf_root}" >&2; exit 1; }
[[ -f "$c6_image" ]] || { echo "C6 image is missing: $c6_image" >&2; exit 1; }
[[ -d "${host_source}/examples/host_performs_slave_ota" ]] || {
	echo "esp-hosted-mcu checkout is missing" >&2
	exit 1
}

rm -rf -- "${output}/project"
rm -rf -- "${output}/build"
mkdir -p -- "${output}/project/components"
cp -a "${host_source}/examples/host_performs_slave_ota/." "${output}/project/"
cp -a "${host_source}/." "${output}/project/components/esp_hosted/"
case "${ES_BOARD:-stamp-p4}" in
	stamp-p4)
		cp -- "${script_dir}/ota-host/sdkconfig.defaults" "${output}/project/sdkconfig.defaults"
		;;
	tab5)
		cp -- "${script_dir}/ota-host/sdkconfig-tab5.defaults" "${output}/project/sdkconfig.defaults"
		mkdir -p -- "${output}/project/components/tab5_c6_power"
		cp -- "${script_dir}/ota-host/tab5/CMakeLists.txt" \
			"${script_dir}/ota-host/tab5/tab5_c6_power.c" \
			"${output}/project/components/tab5_c6_power/"
		;;
	*)
		echo "unsupported ES_BOARD: ${ES_BOARD}" >&2
		exit 2
		;;
esac
cp -- "${script_dir}/ota-host/partitions.csv" "${output}/project/partitions.csv"
mkdir -p -- "${output}/project/components/ota_partition/slave_fw_bin"
cp -- "$c6_image" "${output}/project/components/ota_partition/slave_fw_bin/network_adapter.bin"

export IDF_PATH="$idf_root"
export IDF_TARGET=esp32p4
export SDKCONFIG="${output}/project/sdkconfig"
# Use the ESP-IDF virtualenv explicitly when it is available. The container's
# system Python does not carry the IDF `click` dependency, while the `idf.py`
# wrapper can replace PATH during nested environment activation.
idf_python="${IDF_PYTHON:-}"
if [[ -z "$idf_python" && -x /opt/esp/python_env/idf5.5_py3.12_env/bin/python ]]; then
	idf_python=/opt/esp/python_env/idf5.5_py3.12_env/bin/python
fi
idf_python="${idf_python:-python3}"
idf_cmd=("$idf_python" "${IDF_PATH}/tools/idf.py")

"${idf_cmd[@]}" -C "${output}/project" -B "${output}/build" set-target esp32p4
"${idf_cmd[@]}" -C "${output}/project" -B "${output}/build" reconfigure build

for artifact in bootloader/bootloader.bin partition_table/partition-table.bin host_performs_slave_ota.bin; do
	if [[ ! -s "${output}/build/${artifact}" ]]; then
		echo "missing OTA-host artifact: ${output}/build/${artifact}" >&2
		exit 1
	fi
done
echo "OTA host build complete: ${output}/build"
