#!/usr/bin/env bash
set -euo pipefail

usage() {
	cat >&2 <<'EOF'
Usage: build-ota-host.sh <external-source-root> <c6-network-adapter.bin> <output-directory>

Builds a disposable ESP-IDF ESP32-P4 host that uses ESP-Hosted-MCU over the
Stamp-AddOn C6 SDIO link to update the C6 firmware partition. It never flashes
the P4 or C6. The ESP-Hosted-MCU source is copied from the external cache; no
third-party source is vendored into this repository.
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
cp -- "${script_dir}/ota-host/sdkconfig.defaults" "${output}/project/sdkconfig.defaults"
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
