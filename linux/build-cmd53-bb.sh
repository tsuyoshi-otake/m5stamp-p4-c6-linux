#!/usr/bin/env bash
# CMD53 retention black-box: boot-shim RTC_NOINIT → nm PA → render 0052 → build-m1.
# Fail-closed. Never writes flash. Requires IDF_PATH / idf.py / riscv32-esp-elf-nm.
set -euo pipefail

usage() {
	cat >&2 <<'EOF'
Usage: build-cmd53-bb.sh <external-source-root|--vendor> <build-output> \
         [--profile m1|m2|m3|m3-lab] [--smp] \
         [--selftest|--selftest-torn] [--shim-only] \
         [build-m1 args...]

Maps product profile → boot-shim profile:
  m1              → boot-shim m1
  m2|m3|m3-lab    → boot-shim m2  (EASYSTICK_M2_SDIO / C6 path)

--smp selects the existing C68 Linux SMP handoff and boot-shim profile while
keeping --profile as the network/SSH product profile.  It renders the C68
nm-dependent patches into the same generated override directory as 0052/0053.

When idf.py is missing, uses Docker image espressif/idf:v5.5.3 (override with
EASYSTICK_IDF_DOCKER_IMAGE) if EASYSTICK_IDF_DOCKER=1 (default).

--shim-only stops after boot-shim + nm + rendered 0052 (no build-m1). Use for
retention/torn selftests that only need the shim flashed.

DMA/quiet contract is FORCED (ambient env ignored):
  EASYSTICK_SDIO_FORCE_PIO=0
  EASYSTICK_IDMAC_DESC_INVALIDATE=0
  EASYSTICK_IDMAC_NONCOHERENT_RING=0
  EASYSTICK_CMD53_RX_DESC_BYTES=0
  TX/TCP/SSH ledgers + diagnostics + 0051 = 0

After build-m1 returns, writes cmd53-bb-final-shot-manifest.json.
Shot C is allowed only when selftest=false and selftest_torn=false.
Flash gate: python3 cmd53-bb/final_shot_manifest.py verify --manifest ... --require-shot-c

Set EASYSTICK_ESPHOSTED_CMD52_MARKER=1 to build the P4-only CMD52 marker
positive-control candidate.  The same flag adds six adjacent RTC_NOINIT words
to the boot-shim and renders their PA from that boot-shim's nm output.
EOF
}

if [[ $# -lt 2 ]]; then
	usage
	exit 2
fi

source_arg=$1
output=$(mkdir -p -- "$2" && cd -- "$2" && pwd)
shift 2

product_profile=m3-lab
selftest=0
selftest_torn=0
shim_only=0
smp=0
extra_args=()
while [[ $# -gt 0 ]]; do
	case "$1" in
		--profile)
			[[ $# -ge 2 ]] || { usage; exit 2; }
			product_profile=$2
			shift 2
			;;
		--smp)
			smp=1
			shift
			;;
		--selftest)
			selftest=1
			shift
			;;
		--selftest-torn)
			selftest_torn=1
			shift
			;;
		--shim-only)
			shim_only=1
			shift
			;;
		*)
			extra_args+=("$1")
			shift
			;;
	esac
done

case "$product_profile" in
	m1) shim_profile=m1 ;;
	m2|m3|m3-lab) shim_profile=m2 ;;
	*)
		echo "CMD53_BB_GATE_FAIL: unsupported --profile $product_profile" >&2
		exit 1
		;;
esac
if [[ "$smp" == "1" ]]; then
	shim_profile=c68
fi
if [[ "$selftest" == "1" && "$selftest_torn" == "1" ]]; then
	echo "CMD53_BB_GATE_FAIL: --selftest and --selftest-torn are mutually exclusive" >&2
	exit 1
fi

script_dir=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)
shim_src="${script_dir}/boot-shim"
patch_src="${script_dir}/kernel-patches"
bb_dir="${script_dir}/cmd53-bb"
rendered="${output}/cmd53-bb-rendered-patches"
shim_out="${output}/boot-shim-cmd53-bb"
elf="${shim_out}/build/easystick_stamp_p4_boot_shim.elf"
bin="${shim_out}/build/easystick_stamp_p4_boot_shim.bin"
map_file="${shim_out}/build/easystick_stamp_p4_boot_shim.map"
br_images="${output}/buildroot/images"

command -v sha256sum >/dev/null || {
	echo "CMD53_BB_GATE_FAIL: sha256sum is required" >&2
	exit 1
}

# Git-Bash on this host often resolves python3 to the Windows Store stub (exit 49).
pick_python() {
	if [[ -n "${EASYSTICK_PYTHON:-}" && -x "${EASYSTICK_PYTHON}" ]]; then
		echo "$EASYSTICK_PYTHON"
		return
	fi
	local c
	for c in \
		"/d/Users/Developer/easystick-tmp-20260820/easystick-p4-tools-481/Scripts/python.exe" \
		"/c/Users/developer/AppData/Local/Programs/Python/Python312/python.exe" \
		"python3" "python"; do
		if command -v "$c" >/dev/null 2>&1 || [[ -x "$c" ]]; then
			if "$c" -c "import sys; sys.exit(0 if sys.version_info >= (3, 9) else 1)" 2>/dev/null; then
				echo "$c"
				return
			fi
		fi
	done
	echo "CMD53_BB_GATE_FAIL: no usable Python 3 interpreter" >&2
	exit 1
}
PYTHON=$(pick_python)
echo "CMD53_BB: PYTHON=$PYTHON"

IDF_DOCKER_IMAGE="${EASYSTICK_IDF_DOCKER_IMAGE:-espressif/idf:v5.5.3}"
use_idf_docker=0
if ! command -v idf.py >/dev/null || ! command -v riscv32-esp-elf-nm >/dev/null; then
	if [[ "${EASYSTICK_IDF_DOCKER:-1}" == "1" ]] && command -v docker >/dev/null &&
		docker image inspect "$IDF_DOCKER_IMAGE" >/dev/null 2>&1; then
		use_idf_docker=1
		echo "CMD53_BB: host IDF tools missing; using Docker $IDF_DOCKER_IMAGE for boot-shim"
	else
		echo "CMD53_BB_GATE_FAIL: idf.py/riscv32-esp-elf-nm not on PATH and Docker IDF image unavailable" >&2
		echo "  Install host IDF tools or: docker pull $IDF_DOCKER_IMAGE" >&2
		exit 1
	fi
fi

# Docker Desktop on Git-Bash/MSYS rewrites -v paths unless MSYS_NO_PATHCONV=1.
docker_host_path() {
	local p=$1
	if command -v cygpath >/dev/null 2>&1; then
		cygpath -w "$p"
	elif [[ "$p" =~ ^/([a-zA-Z])/(.*)$ ]]; then
		echo "${BASH_REMATCH[1]^}:\\${BASH_REMATCH[2]//\//\\}"
	else
		echo "$p"
	fi
}

build_boot_shim() {
	local profile=$1
	local out_dir=$2
	mkdir -p -- "$out_dir"
	if [[ "$use_idf_docker" == "1" ]]; then
		local host_linux host_out
		host_linux=$(docker_host_path "$script_dir")
		host_out=$(docker_host_path "$out_dir")
		MSYS_NO_PATHCONV=1 docker run --rm \
			-v "${host_linux}:/src/linux:ro" \
			-v "${host_out}:/out" \
			-e EASYSTICK_CMD53_RETENTION_BB \
			-e EASYSTICK_CMD52_MARKER \
			-e EASYSTICK_C68_CLEAN_RELEASE \
			-e EASYSTICK_CMD53_BB_SELFTEST \
			-e EASYSTICK_CMD53_BB_SELFTEST_TORN \
			-e SHIM_PROFILE="$profile" \
			"$IDF_DOCKER_IMAGE" bash -lc '
set -euo pipefail
mkdir -p /work
if [[ "$SHIM_PROFILE" == c68 ]]; then
	rm -rf /work/boot-shim
	mkdir -p /work/boot-shim
	tar -C /src/linux/boot-shim \
		--exclude=sdkconfig --exclude=sdkconfig.old --exclude=build \
		-cf - . | tar -C /work/boot-shim -xf -
	export EASYSTICK_C68_CLEAN_RELEASE=1
else
	cp -a /src/linux/boot-shim /work/boot-shim
fi
cp -a /src/linux/cmd53-bb /work/cmd53-bb
export IDF_TARGET=esp32p4
export EASYSTICK_BOOT_PROFILE="$SHIM_PROFILE"
export SDKCONFIG=/out/sdkconfig
if [[ "$SHIM_PROFILE" == m2 || "$SHIM_PROFILE" == c68 ]]; then
	export SDKCONFIG_DEFAULTS="/work/boot-shim/sdkconfig.defaults;/work/boot-shim/sdkconfig-m2.defaults"
else
	export SDKCONFIG_DEFAULTS="/work/boot-shim/sdkconfig.defaults"
fi
rm -f /out/sdkconfig /out/sdkconfig.old
idf.py -B /out/build -C /work/boot-shim reconfigure build
for artifact in bootloader/bootloader.bin partition_table/partition-table.bin easystick_stamp_p4_boot_shim.bin; do
	[[ -s "/out/build/${artifact}" ]] || { echo "missing $artifact" >&2; exit 1; }
done
'
	else
		"${shim_src}/build.sh" "$out_dir" --profile "$profile"
	fi
}

nm_boot_shim() {
	local elf_path=$1
	if [[ "$use_idf_docker" == "1" ]]; then
		local elf_dir elf_base host_elf
		elf_dir=$(cd -- "$(dirname -- "$elf_path")" && pwd)
		elf_base=$(basename -- "$elf_path")
		host_elf=$(docker_host_path "$elf_dir")
		MSYS_NO_PATHCONV=1 docker run --rm -v "${host_elf}:/elf:ro" "$IDF_DOCKER_IMAGE" \
			bash -lc "riscv32-esp-elf-nm -S /elf/${elf_base}"
	else
		riscv32-esp-elf-nm -S "$elf_path"
	fi
}

# --- Forced DMA / quiet contract (do not inherit ambient experiment flags) ---
boot_marker="${EASYSTICK_CMD52_MARKER:-}"
host_marker="${EASYSTICK_ESPHOSTED_CMD52_MARKER:-}"
if [[ -n "$boot_marker" && -n "$host_marker" && "$boot_marker" != "$host_marker" ]]; then
	echo "CMD52_MARKER_GATE_FAIL: boot-shim and esp-hosted marker flags differ" >&2
	exit 1
fi
cmd52_marker="${host_marker:-${boot_marker:-0}}"
case "$cmd52_marker" in
	0|1) ;;
	*) echo "CMD52_MARKER_GATE_FAIL: marker flag must be 0 or 1" >&2; exit 1 ;;
esac
export EASYSTICK_CMD52_MARKER="$cmd52_marker"
export EASYSTICK_ESPHOSTED_CMD52_MARKER="$cmd52_marker"
export EASYSTICK_CMD53_RETENTION_BB=1
export EASYSTICK_SDIO_FORCE_PIO=0
export EASYSTICK_IDMAC_DESC_INVALIDATE=0
export EASYSTICK_IDMAC_NONCOHERENT_RING=0
export EASYSTICK_CMD53_RX_DESC_BYTES=0
export EASYSTICK_ESPHOSTED_TX_LEDGER=0
export EASYSTICK_TCP22_LEDGER=0
export EASYSTICK_SSH_LEDGER=0
export EASYSTICK_ESPHOSTED_DIAGNOSTICS=0
export EASYSTICK_DW_MMC_CMD53_ERR_PROV=0

unset EASYSTICK_CMD53_BB_SELFTEST EASYSTICK_CMD53_BB_SELFTEST_TORN || true
if [[ "$selftest_torn" == "1" ]]; then
	export EASYSTICK_CMD53_BB_SELFTEST_TORN=1
elif [[ "$selftest" == "1" ]]; then
	export EASYSTICK_CMD53_BB_SELFTEST=1
fi

echo "CMD53_BB: product_profile=$product_profile boot_shim_profile=$shim_profile FORCE_PIO=0 cmd52_marker=$cmd52_marker"
build_boot_shim "$shim_profile" "$shim_out"
[[ -f "$elf" && -f "$bin" ]] || {
	echo "CMD53_BB_GATE_FAIL: missing $elf or $bin" >&2
	exit 1
}

if [[ "$shim_profile" == "m2" ]]; then
	grep -aFq 'C6 reset GPIO42' "$elf" || \
		grep -aFq 'Slot 1 SDIO GPIO matrix ready' "$elf" || {
		echo "CMD53_BB_GATE_FAIL: m2 boot-shim missing EASYSTICK_M2_SDIO markers" >&2
		exit 1
	}
fi

nm_out=$(nm_boot_shim "$elf")
printf '%s\n' "$nm_out" >"${output}/cmd53-bb.nm"
if [[ "$shim_profile" == "c68" ]]; then
	nm_release=$(awk '/ c68_release$/{print $1; exit}' <<<"$nm_out")
	nm_stage=$(awk '/ c68_stage$/{print $1; exit}' <<<"$nm_out")
	nm_entry=$(awk '/ c68_secondary_entry$/{print $1; exit}' <<<"$nm_out")
	[[ -n "$nm_release" && -n "$nm_stage" && -n "$nm_entry" ]] || {
		echo "C68_GATE_FAIL: nm missing c68_release/c68_stage/c68_secondary_entry" >&2
		exit 1
	}
	c68_release_pa="0x${nm_release}u"
	c68_stage_pa="0x${nm_stage}u"
	c68_entry_pa="0x${nm_entry}"
	printf '%s\n' "$c68_release_pa" >"${output}/c68-release.pa"
	printf '%s\n' "$c68_stage_pa" >"${output}/c68-stage.pa"
	printf '%s\n' "$c68_entry_pa" >"${output}/c68-entry.pa"
	echo "C68_NM release=$c68_release_pa stage=$c68_stage_pa entry=$c68_entry_pa"
fi
assert_args=("${output}/cmd53-bb.nm")
if [[ -f "$map_file" ]]; then
	assert_args+=("$map_file")
fi
if [[ "$cmd52_marker" == "1" ]]; then
	assert_args+=(--require-cmd52-marker)
fi
"$PYTHON" "${bb_dir}/assert_rtc_noinit.py" "${assert_args[@]}" \
	| tee "${output}/cmd53-bb-assert.txt"
bb_pa=$(grep -E '^0x[0-9a-fA-F]+u$' "${output}/cmd53-bb-assert.txt" | tail -n1)
[[ -n "$bb_pa" ]] || {
	echo "CMD53_BB_GATE_FAIL: could not parse PA from assert script" >&2
	exit 1
}
echo "CMD53_BB_PA=$bb_pa"
printf '%s\n' "$bb_pa" >"${output}/cmd53-bb.pa"
bb_size=$(grep -E '^CMD53_BB_SIZE=[0-9]+$' "${output}/cmd53-bb-assert.txt" |
	awk -F= 'END { print $2 }')
[[ "$bb_size" == "288" ]] || {
	echo "CMD53_BB_GATE_FAIL: expected exact BB size 288, got ${bb_size:-missing}" >&2
	exit 1
}
echo "CMD53_BB_SIZE=$bb_size"
printf '%s\n' "$bb_size" >"${output}/cmd53-bb.size"

elf_sha=$(sha256sum "$elf" | awk '{print $1}')
bin_sha=$(sha256sum "$bin" | awk '{print $1}')
printf '%s\n' "$elf_sha" >"${output}/cmd53-bb-elf.sha256"
printf '%s\n' "$bin_sha" >"${output}/cmd53-bb-bin.sha256"

rm -rf -- "$rendered"
mkdir -p -- "$rendered"
if [[ "$shim_profile" == "c68" ]]; then
	"$PYTHON" "${script_dir}/c68/render_patches.py" \
		"$patch_src" "$rendered" \
		"$c68_release_pa" "$c68_stage_pa"
fi
for patch_name in \
	0052-easystick-dw-mmc-cmd53-retention-bb.patch \
	0053-easystick-mmc-cmd53-post-bb.patch \
	0054-easystick-wdt-crash-capsule.patch; do
	"$PYTHON" "${bb_dir}/render_patch.py" \
		"${patch_src}/${patch_name}" \
		"${rendered}/${patch_name}" \
		"$bb_pa"
done
"$PYTHON" "${bb_dir}/render_patch.py" \
	"${bb_dir}/esp-hosted-patches/0026-easystick-cmd53-retention-bb6.patch" \
	"${rendered}/0026-easystick-cmd53-retention-bb6.patch" \
	"$bb_pa"
if [[ "$cmd52_marker" == "1" ]]; then
	"$PYTHON" "${bb_dir}/render_patch.py" \
		"${script_dir}/buildroot-external/package/esp-hosted-ng/0030-easystick-sdio-cmd52-retention-marker.patch" \
		"${rendered}/0030-easystick-sdio-cmd52-retention-marker.patch" \
		"$bb_pa"
fi
rendered_sha=$(sha256sum "${rendered}/0052-easystick-dw-mmc-cmd53-retention-bb.patch" | awk '{print $1}')
rendered_0053_sha=$(sha256sum "${rendered}/0053-easystick-mmc-cmd53-post-bb.patch" | awk '{print $1}')
rendered_0054_sha=$(sha256sum "${rendered}/0054-easystick-wdt-crash-capsule.patch" | awk '{print $1}')
rendered_bb6_sha=$(sha256sum "${rendered}/0026-easystick-cmd53-retention-bb6.patch" | awk '{print $1}')
rendered_marker_sha=none
rendered_marker_path=none
if [[ "$cmd52_marker" == "1" ]]; then
	rendered_marker_path="${rendered}/0030-easystick-sdio-cmd52-retention-marker.patch"
	rendered_marker_sha=$(sha256sum \
		"$rendered_marker_path" |
		awk '{print $1}')
fi
printf '%s\n' "$rendered_sha" >"${output}/cmd53-bb-0052.sha256"
printf '%s\n' "$rendered_0053_sha" >"${output}/cmd53-bb-0053.sha256"
printf '%s\n' "$rendered_0054_sha" >"${output}/cmd53-bb-0054.sha256"
printf '%s\n' "$rendered_bb6_sha" >"${output}/cmd53-bb-0026.sha256"
printf '%s\n' "$rendered_marker_sha" >"${output}/cmd53-bb-0030.sha256"

"$PYTHON" - "$output/cmd53-bb-manifest.json" \
	"$product_profile" "$shim_profile" "$bb_pa" \
	"$bin" "$bin_sha" "$elf" "$elf_sha" \
	"$rendered/0052-easystick-dw-mmc-cmd53-retention-bb.patch" "$rendered_sha" \
	"$rendered/0053-easystick-mmc-cmd53-post-bb.patch" "$rendered_0053_sha" \
	"$rendered/0054-easystick-wdt-crash-capsule.patch" "$rendered_0054_sha" \
	"$rendered/0026-easystick-cmd53-retention-bb6.patch" "$rendered_bb6_sha" \
	"$selftest" "$selftest_torn" "$bb_size" "$cmd52_marker" \
	"$rendered_marker_path" "$rendered_marker_sha" <<'PY'
import re
import json, sys
from pathlib import Path
(out, product, shim, pa, bin_path, bin_sha, elf_path, elf_sha,
 patch52, patch52_sha, patch53, patch53_sha, patch54, patch54_sha,
 patch26, patch26_sha, st, torn, bb_size, marker_enabled,
 marker_path, marker_sha) = sys.argv[1:]
def bb_version(path):
    text = Path(path).read_text(encoding="utf-8", errors="replace")
    values = re.findall(
        r"#define\s+EASYSTICK_CMD53_BB_VERSION\s+([0-9]+)u\b", text
    )
    if len(set(values)) != 1:
        raise SystemExit("CMD53_BB_GATE_FAIL: inconsistent BB version in patch")
    return int(values[0])
version = bb_version(patch52)
if version != 6:
    raise SystemExit(f"CMD53_BB_GATE_FAIL: unexpected BB version {version}")
if int(bb_size) != 0x120:
    raise SystemExit(f"CMD53_BB_GATE_FAIL: unexpected BB size {bb_size}")
for patch in (patch53, patch26):
    if bb_version(patch) != version:
        raise SystemExit("CMD53_BB_GATE_FAIL: rendered patch BB version mismatch")
doc = {
    "product_profile": product,
    "boot_shim_profile": shim,
    "easystick_cmd53_bb_pa": pa,
    "easystick_cmd53_bb_version": version,
    "easystick_cmd53_bb_size": int(bb_size),
    "cmd52_marker_enabled": marker_enabled == "1",
    "cmd52_marker_pa": (
        f"0x{int(pa.rstrip('u'), 16) + 0x120:08x}u"
        if marker_enabled == "1" else None
    ),
    "cmd52_marker_words_count": 6 if marker_enabled == "1" else None,
    "cmd52_marker_size": 24 if marker_enabled == "1" else None,
    "cmd52_marker_words": {
        "magic": "0x45534d30",
        "armed": "0x45534d31",
        "token_enter": "0x45534d32",
        "after_46": "0x45534d33",
        "before_47": "0x45534d34",
        "after_47": "0x45534d35",
    },
    "crash_capsule_offset": 0x80,
    "request_end_markers": [
        "stage_request_end_enter",
        "stage_request_end_before_next",
        "stage_request_end_after_next",
        "stage_request_end_idle",
    ],
    "boot_shim_bin": bin_path,
    "boot_shim_bin_sha256": bin_sha,
    "boot_shim_elf": elf_path,
    "boot_shim_elf_sha256": elf_sha,
    "rendered_0052": patch52,
    "rendered_0052_sha256": patch52_sha,
    "rendered_0053": patch53,
    "rendered_0053_sha256": patch53_sha,
    "rendered_0054": patch54,
    "rendered_0054_sha256": patch54_sha,
    "rendered_0026_esp_hosted": patch26,
    "rendered_0026_esp_hosted_sha256": patch26_sha,
    "selftest": st == "1",
    "selftest_torn": torn == "1",
    "dma_contract_forced": True,
    "experiment": "0053-post-mmc_request_done",
}
if marker_enabled == "1":
    marker_file = Path(marker_path)
    if not marker_file.is_file():
        raise SystemExit("CMD52_MARKER_GATE_FAIL: rendered marker patch missing")
    if "BBDEAD" in marker_file.read_text(encoding="utf-8", errors="replace"):
        raise SystemExit("CMD52_MARKER_GATE_FAIL: placeholder remains in marker patch")
    doc["rendered_0030_cmd52_marker"] = str(marker_file)
    doc["rendered_0030_cmd52_marker_sha256"] = marker_sha
Path(out).write_text(json.dumps(doc, indent=2) + "\n", encoding="utf-8")
print(f"CMD53_BB_MANIFEST {out}")
PY

profile_args=()
have_profile=0
for a in "${extra_args[@]+"${extra_args[@]}"}"; do
	if [[ "$a" == --profile ]]; then
		have_profile=1
	fi
done
if [[ "$have_profile" -eq 0 ]]; then
	profile_args=(--profile "$product_profile")
fi

export EASYSTICK_KERNEL_PATCH_OVERRIDE_DIR="$rendered"
export EASYSTICK_CMD53_BB_PA="$bb_pa"
export EASYSTICK_CMD53_BB_0052_SHA256="$rendered_sha"
export EASYSTICK_CMD53_BB_0053_SHA256="$rendered_0053_sha"
export EASYSTICK_CMD53_BB_0054_SHA256="$rendered_0054_sha"
export EASYSTICK_CMD53_BB_ESPHOSTED_PATCH="${rendered}/0026-easystick-cmd53-retention-bb6.patch"
if [[ "$cmd52_marker" == "1" ]]; then
	export EASYSTICK_CMD53_BB_ESPHOSTED_MARKER_PATCH="$rendered_marker_path"
else
	unset EASYSTICK_CMD53_BB_ESPHOSTED_MARKER_PATCH || true
fi
export EASYSTICK_CMD53_RETENTION_BB=1
export EASYSTICK_CMD53_BOOT_SHIM_BIN="$bin"
export EASYSTICK_CMD53_BOOT_SHIM_ELF="$elf"
export EASYSTICK_CMD53_BOOT_SHIM_BIN_SHA256="$bin_sha"
export EASYSTICK_CMD53_BOOT_SHIM_ELF_SHA256="$elf_sha"
if [[ "$shim_profile" == "c68" ]]; then
	export EASYSTICK_C68_CLEAN_RELEASE=1
	export EASYSTICK_C68_RELEASE_PA="$c68_release_pa"
	export EASYSTICK_C68_STAGE_PA="$c68_stage_pa"
	export EASYSTICK_C68_ENTRY_PA="$c68_entry_pa"
	export EASYSTICK_C68_BOOT_SHIM_BIN="$bin"
	export EASYSTICK_C68_BOOT_SHIM_ELF="$elf"
fi

if [[ "$shim_only" == "1" ]]; then
	echo "CMD53_BB: --shim-only complete (PA=$bb_pa bin=$bin)"
	echo "CMD53_BB: flash bootloader+ptable+app; do not treat as shot_c_allowed"
	exit 0
fi

# Do not exec: return here to emit the fail-closed final-shot manifest.
"${script_dir}/build-m1.sh" "$source_arg" "$output" "${profile_args[@]}" ${extra_args[@]+"${extra_args[@]}"}

image="${br_images}/Image"
dtb="${br_images}/easystick-stamp-p4.dtb"
rootfs=""
for cand in \
	"${br_images}/rootfs.squashfs" \
	"${br_images}/rootfs.ext2" \
	"${br_images}/rootfs.cpio" \
	"${br_images}/rootfs.tar"; do
	if [[ -f "$cand" ]]; then
		rootfs=$cand
		break
	fi
done
[[ -n "$rootfs" ]] || {
	echo "CMD53_BB_GATE_FAIL: no rootfs artifact under ${br_images}" >&2
	ls -la "$br_images" >&2 || true
	exit 1
}

if [[ "${EASYSTICK_CMD53_RETENTION_BB:-0}" == "1" ]]; then
	wdt_attest="${output}/wdt-crash-capsule-attestation.json"
	[[ -s "$wdt_attest" ]] || {
		echo "CMD53_BB_GATE_FAIL: missing WDT crash-capsule build attestation" >&2
		exit 1
	}
	"$PYTHON" - "$wdt_attest" "$rendered_0054_sha" "$image" <<'PY'
import hashlib
import json
import sys
from pathlib import Path

attest_path, expected_patch_sha, image_path = sys.argv[1:]
doc = json.loads(Path(attest_path).read_text(encoding="utf-8"))
if doc.get("kind") != "easystick-wdt-crash-capsule-build-attestation":
    raise SystemExit("CMD53_BB_GATE_FAIL: invalid WDT attestation kind")
actual_patch_sha = doc.get("patch", {}).get("sha256", "").lower()
if actual_patch_sha != expected_patch_sha.lower():
    raise SystemExit(
        "CMD53_BB_GATE_FAIL: WDT attestation patch SHA does not match rendered 0054"
    )
digest = hashlib.sha256()
with open(image_path, "rb") as stream:
    for chunk in iter(lambda: stream.read(1024 * 1024), b""):
        digest.update(chunk)
actual_image_sha = digest.hexdigest()
if doc.get("Image", {}).get("sha256", "").lower() != actual_image_sha:
    raise SystemExit(
        "CMD53_BB_GATE_FAIL: WDT attestation Image SHA does not match built Image"
    )
for section in ("source", "vmlinux", "Image"):
    if not doc.get(section, {}).get("sha256"):
        raise SystemExit(
            f"CMD53_BB_GATE_FAIL: WDT attestation has no {section} SHA256"
        )
print(f"CMD53_BB_WDT_ATTEST_OK patch={actual_patch_sha} image={actual_image_sha}")
PY
fi

write_args=(
	write
	--out "${output}/cmd53-bb-final-shot-manifest.json"
	--pa "$bb_pa"
	--product-profile "$product_profile"
	--boot-shim-profile "$shim_profile"
	--boot-shim-bin "$bin"
	--image "$image"
	--dtb "$dtb"
	--rootfs "$rootfs"
	--rendered-0052 "${rendered}/0052-easystick-dw-mmc-cmd53-retention-bb.patch"
	--rendered-0053 "${rendered}/0053-easystick-mmc-cmd53-post-bb.patch"
	--rendered-0054 "${rendered}/0054-easystick-wdt-crash-capsule.patch"
	--rendered-0026 "${rendered}/0026-easystick-cmd53-retention-bb6.patch"
	--bb-size "$bb_size"
	--idmac-noncoherent-ring "${EASYSTICK_IDMAC_NONCOHERENT_RING:-0}"
)
if [[ "$cmd52_marker" == "1" ]]; then
	write_args+=(--rendered-0030 "$rendered_marker_path" --cmd52-marker)
fi
if [[ "$selftest" == "1" ]]; then
	write_args+=(--selftest)
fi
if [[ "$selftest_torn" == "1" ]]; then
	write_args+=(--selftest-torn)
fi
"$PYTHON" "${bb_dir}/final_shot_manifest.py" "${write_args[@]}"

# Immediate self-verify of the just-written final manifest.
verify_args=(verify --manifest "${output}/cmd53-bb-final-shot-manifest.json")
if [[ "$selftest" == "0" && "$selftest_torn" == "0" ]]; then
	verify_args+=(--require-shot-c)
fi
"$PYTHON" "${bb_dir}/final_shot_manifest.py" "${verify_args[@]}"

if [[ "$selftest" == "0" && "$selftest_torn" == "0" ]]; then
	echo "CMD53_BB: final non-selftest candidate ready (shot_c_allowed=true)"
else
	echo "CMD53_BB: selftest build complete (shot_c_allowed=false; rebuild without --selftest* for C)"
fi
