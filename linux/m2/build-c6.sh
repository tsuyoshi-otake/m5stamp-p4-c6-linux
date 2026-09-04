#!/usr/bin/env bash
set -euo pipefail

usage() {
	cat >&2 <<'EOF'
Usage: build-c6.sh <external-source-root|--vendor> <output-directory>

Builds the locked ESP-Hosted-NG ESP32-C6 network_adapter slave image for
SDIO. IDF_PATH must point at the locked ESP-IDF checkout (or be passed by the
container wrapper). This command never invokes esptool or writes the C6.
EOF
}

[[ $# -eq 2 ]] || { usage; exit 2; }
source_arg=$1
output=$(mkdir -p -- "$2" && cd -- "$2" && pwd)
script_dir=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)
firmware_root=$(cd -- "${script_dir}/../.." && pwd)

if [[ "$source_arg" == "--vendor" ]]; then
	source_root="${firmware_root}/vendor"
	hosted_root="${source_root}/esp-hosted/esp_hosted_ng"
	idf_default="${source_root}/esp-idf"
else
	source_root=$(cd -- "$source_arg" && pwd)
	hosted_root="${source_root}/esp_hosted_ng/esp_hosted_ng"
	idf_default="${source_root}/esp_idf"
fi
project_dir="${hosted_root}/esp/esp_driver/network_adapter"
hosted_driver_root="${hosted_root}/esp/esp_driver"
export IDF_PATH="${IDF_PATH:-$idf_default}"
[[ -f "${IDF_PATH}/tools/idf.py" && -f "${project_dir}/CMakeLists.txt" ]] || {
	echo "locked ESP-IDF or network_adapter project is missing" >&2
	exit 1
}

# ESP-Hosted-NG's C6 application calls MLME-offload symbols that stock
# ESP-IDF does not export.  The upstream setup script supplies a pinned
# esp32c6 Wi-Fi library set and a small ROM linker patch.  Prepare those files
# only in this disposable build environment, and restore IDF_PATH on exit so a
# developer checkout is never left modified.
hosted_wifi_lib="${hosted_driver_root}/lib/esp32c6"
hosted_rom_patch="${hosted_driver_root}/lib/rom.patch"
[[ -d "$hosted_wifi_lib" && -f "$hosted_wifi_lib/libnet80211.a" && -f "$hosted_rom_patch" ]] || {
	echo "ESP-Hosted-NG C6 library set or ROM patch is missing" >&2
	exit 1
}
wifi_lib_target="${IDF_PATH}/components/esp_wifi/lib/esp32c6"
wifi_lib_backup="${output}/.stock-esp32c6-wifi-lib"
rom_patch_applied=0
rm -rf -- "$wifi_lib_backup"
cp -a -- "$wifi_lib_target" "$wifi_lib_backup"
restore_idf() {
	if [[ "$rom_patch_applied" == 1 ]]; then
		git -C "$IDF_PATH" apply --reverse "$hosted_rom_patch" >/dev/null 2>&1 || true
	fi
	rm -rf -- "$wifi_lib_target"
	mv -- "$wifi_lib_backup" "$wifi_lib_target" 2>/dev/null || true
}
trap restore_idf EXIT
if git -C "$IDF_PATH" apply --check "$hosted_rom_patch" >/dev/null 2>&1; then
	git -C "$IDF_PATH" apply "$hosted_rom_patch"
	rom_patch_applied=1
fi
rm -rf -- "$wifi_lib_target"
mkdir -p -- "$(dirname -- "$wifi_lib_target")"
cp -a -- "$hosted_wifi_lib" "$wifi_lib_target"

# Do not let a stale sdkconfig/build directory from the upstream checkout
# silently select the wrong transport. Build a disposable project copy.
staged_project="${output}/project"
rm -rf -- "$staged_project"
mkdir -p -- "$staged_project"
cp -a -- "$project_dir/." "$staged_project/"
rm -f -- "$staged_project/sdkconfig"
rm -rf -- "$staged_project/build"

# Patches are LF. Windows checkouts may still carry CRLF into the bind mount.
find "$staged_project" -type f \( -name '*.c' -o -name '*.h' -o -name '*.txt' -o -name '*.md' \) \
	-exec sed -i 's/\r$//' {} +

# EasyStick C6-only patches (never mutate the locked submodule tree).
patch_dir="${script_dir}/c6-patches"
manifest="${output}/C6_PATCH_MANIFEST.txt"
{
	echo "source_project=${project_dir}"
	echo "staged_project=${staged_project}"
	echo "patch_dir=${patch_dir}"
} >"$manifest"
shopt -s nullglob
patch_files=("${patch_dir}"/*.patch)
if ((${#patch_files[@]} > 0)); then
	command -v patch >/dev/null 2>&1 || {
		echo "patch(1) is required to apply ${patch_dir}" >&2
		exit 1
	}
	for patch_file in "${patch_files[@]}"; do
		echo "applying C6 patch: ${patch_file}"
		patch -d "$staged_project" -p1 --forward --batch <"$patch_file"
		if command -v sha256sum >/dev/null 2>&1; then
			sha256sum -- "$patch_file" >>"$manifest"
		elif command -v shasum >/dev/null 2>&1; then
			shasum -a 256 -- "$patch_file" >>"$manifest"
		else
			echo "patch=${patch_file}" >>"$manifest"
		fi
	done
else
	echo "no C6 patches under ${patch_dir}" >>"$manifest"
fi
shopt -u nullglob

idf_cmd=(idf.py)
if ! command -v idf.py >/dev/null 2>&1; then
	idf_cmd=(python3 "${IDF_PATH}/tools/idf.py")
fi
export IDF_TARGET=esp32c6
export SDKCONFIG="${output}/sdkconfig"

"${idf_cmd[@]}" -C "$staged_project" -B "${output}/build" set-target esp32c6
"${idf_cmd[@]}" -C "$staged_project" -B "${output}/build" reconfigure build

for artifact in bootloader/bootloader.bin partition_table/partition-table.bin network_adapter.bin; do
	[[ -s "${output}/build/${artifact}" ]] || {
		echo "missing C6 artifact: ${output}/build/${artifact}" >&2
		exit 1
	}
done
grep -Eq '^#define CONFIG_ESP_SDIO_HOST_INTERFACE 1$' "${output}/build/config/sdkconfig.h" || {
	echo "C6 build did not select SDIO transport" >&2
	exit 1
}
if grep -Eq '^#define CONFIG_ESP_SPI_HOST_INTERFACE 1$' "${output}/build/config/sdkconfig.h"; then
	echo "C6 build unexpectedly selected SPI transport" >&2
	exit 1
fi

# Identity is binary + patch SHA, not NG version bump (keep 1.0.6.0.1).
na_bin="${output}/build/network_adapter.bin"
{
	echo "network_adapter.bin=${na_bin}"
	if command -v sha256sum >/dev/null 2>&1; then
		sha256sum -- "$na_bin"
	elif command -v shasum >/dev/null 2>&1; then
		shasum -a 256 -- "$na_bin"
	fi
	if grep -Eq 'PROJECT_REVISION_PATCH_2[[:space:]]+1' \
		"${staged_project}/main/include/esp_fw_version.h"; then
		echo "version_guard=NG-1.0.6.0.1_PATCH_2_ok"
	else
		echo "version_guard=UNEXPECTED" >&2
		exit 1
	fi
} >>"$manifest"
echo "C6 SDIO slave build complete: ${output}/build"
echo "manifest: ${manifest}"
