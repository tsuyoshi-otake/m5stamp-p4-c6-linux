#!/usr/bin/env python3
"""Write / verify the fail-closed final-shot manifest for Experiment C."""
from __future__ import annotations

import argparse
import hashlib
import json
import re
import sys
from pathlib import Path


EXPECTED_BB_VERSION = 6
EXPECTED_BB_SIZE = 0x120
EXPECTED_CMD52_MARKER_OFFSET = 0x120
EXPECTED_CMD52_MARKER_WORDS = 6
EXPECTED_CMD52_MARKER_SIZE = EXPECTED_CMD52_MARKER_WORDS * 4
EXPECTED_CMD52_MARKER_VALUES = {
    "EASYSTICK_CMD52_MARKER_MAGIC": 0x45534D30,
    "EASYSTICK_CMD52_MARKER_ARMED": 0x45534D31,
    "EASYSTICK_CMD52_MARKER_TOKEN_ENTER": 0x45534D32,
    "EASYSTICK_CMD52_MARKER_AFTER_46": 0x45534D33,
    "EASYSTICK_CMD52_MARKER_BEFORE_47": 0x45534D34,
    "EASYSTICK_CMD52_MARKER_AFTER_47": 0x45534D35,
}


def sha256_file(path: Path) -> str:
    h = hashlib.sha256()
    with path.open("rb") as f:
        for chunk in iter(lambda: f.read(1024 * 1024), b""):
            h.update(chunk)
    return h.hexdigest()


def extract_define_u32(patch: Path, name: str) -> int:
    text = patch.read_text(encoding="utf-8", errors="replace")
    matches = re.findall(
        rf"#define\s+{re.escape(name)}\s+((?:0x[0-9a-fA-F]+)|(?:[0-9]+))u\b",
        text,
    )
    if not matches:
        raise SystemExit(f"CMD53_BB_GATE_FAIL: rendered patch missing {name}")
    if len(set(matches)) != 1:
        raise SystemExit(f"CMD53_BB_GATE_FAIL: rendered patch has conflicting {name}")
    return int(matches[0], 0)


def extract_pa_from_patch(patch: Path) -> str:
    text = patch.read_text(encoding="utf-8", errors="replace")
    if "BBDEAD" in text:
        raise SystemExit("CMD53_BB_GATE_FAIL: BBDEAD remains in rendered 0052")
    return f"0x{extract_define_u32(patch, 'EASYSTICK_CMD53_BB_PA'):08x}u"


def write_manifest(args: argparse.Namespace) -> int:
    image = Path(args.image)
    dtb = Path(args.dtb)
    rootfs = Path(args.rootfs)
    boot_bin = Path(args.boot_shim_bin)
    rendered = Path(args.rendered_0052)
    for p in (image, dtb, rootfs, boot_bin, rendered):
        if not p.is_file():
            raise SystemExit(f"CMD53_BB_GATE_FAIL: missing artifact {p}")

    pa = args.pa
    patch_pa = extract_pa_from_patch(rendered)
    if pa != patch_pa:
        raise SystemExit(
            f"CMD53_BB_GATE_FAIL: PA mismatch nm={pa} rendered={patch_pa}"
        )
    bb_version = extract_define_u32(rendered, "EASYSTICK_CMD53_BB_VERSION")
    if bb_version != EXPECTED_BB_VERSION:
        raise SystemExit(
            f"CMD53_BB_GATE_FAIL: unexpected BB version {bb_version}"
        )
    if args.bb_size != EXPECTED_BB_SIZE:
        raise SystemExit(
            f"CMD53_BB_GATE_FAIL: unexpected BB size {args.bb_size}"
        )
    marker_patch = None
    if args.cmd52_marker:
        if not args.rendered_0030:
            raise SystemExit(
                "CMD52_MARKER_GATE_FAIL: marker enabled without rendered 0030"
            )
        marker_patch = Path(args.rendered_0030)
        if not marker_patch.is_file():
            raise SystemExit(
                f"CMD52_MARKER_GATE_FAIL: missing artifact {marker_patch}"
            )
        if extract_pa_from_patch(marker_patch) != pa:
            raise SystemExit(
                "CMD52_MARKER_GATE_FAIL: PA mismatch in rendered 0030"
            )
        if extract_define_u32(
            marker_patch, "EASYSTICK_CMD52_MARKER_OFFSET"
        ) != EXPECTED_CMD52_MARKER_OFFSET:
            raise SystemExit(
                "CMD52_MARKER_GATE_FAIL: unexpected marker offset"
            )
        if extract_define_u32(
            marker_patch, "EASYSTICK_CMD52_MARKER_WORDS"
        ) != EXPECTED_CMD52_MARKER_WORDS:
            raise SystemExit(
                "CMD52_MARKER_GATE_FAIL: unexpected marker word count"
            )
        for name, expected in EXPECTED_CMD52_MARKER_VALUES.items():
            if extract_define_u32(marker_patch, name) != expected:
                raise SystemExit(
                    f"CMD52_MARKER_GATE_FAIL: unexpected value for {name}"
                )

    image_sha = sha256_file(image)
    # Fail closed if placeholder survived into the linked Image.
    image_bytes = image.read_bytes()
    if b"BBDEAD" in image_bytes:
        raise SystemExit("CMD53_BB_GATE_FAIL: BBDEAD found in Image")
    pa_int = int(pa.rstrip("u"), 16)
    pa_le = pa_int.to_bytes(4, "little")
    # Absolute u32 literal (some compilers), or RISC-V lui+addi split.
    # For RV32, unsigned PA 0x50108080 is typically: lui rd,0x50108; addi/lw with +0x80.
    lui_imm = pa_int >> 12
    lui_hit = False
    for rd in range(32):
        insn = (lui_imm << 12) | (rd << 7) | 0x37
        if insn.to_bytes(4, "little") in image_bytes:
            lui_hit = True
            break
    if pa_le not in image_bytes and not lui_hit:
        raise SystemExit(
            f"CMD53_BB_GATE_FAIL: Image missing PA {pa} "
            f"(neither LE u32 nor RISC-V lui imm 0x{lui_imm:x})"
        )

    selftest = args.selftest
    selftest_torn = args.selftest_torn
    shot_c_allowed = (not selftest) and (not selftest_torn)

    doc = {
        "kind": "cmd53-bb-final-shot",
        "shot_c_allowed": shot_c_allowed,
        "product_profile": args.product_profile,
        "boot_shim_profile": args.boot_shim_profile,
        "easystick_cmd53_bb_pa": pa,
        "easystick_cmd53_bb_version": bb_version,
        "easystick_cmd53_bb_size": args.bb_size,
        "cmd52_marker_enabled": args.cmd52_marker,
        "cmd52_marker_pa": (
            f"0x{int(pa.rstrip('u'), 16) + EXPECTED_CMD52_MARKER_OFFSET:08x}u"
            if args.cmd52_marker else None
        ),
        "cmd52_marker_words_count": (
            EXPECTED_CMD52_MARKER_WORDS if args.cmd52_marker else None
        ),
        "cmd52_marker_size": (
            EXPECTED_CMD52_MARKER_SIZE if args.cmd52_marker else None
        ),
        "cmd52_marker_words": {
            "magic": "0x45534d30",
            "armed": "0x45534d31",
            "token_enter": "0x45534d32",
            "after_46": "0x45534d33",
            "before_47": "0x45534d34",
            "after_47": "0x45534d35",
        },
        "selftest": selftest,
        "selftest_torn": selftest_torn,
        "dma_contract": {
            "EASYSTICK_SDIO_FORCE_PIO": "0",
            "EASYSTICK_IDMAC_DESC_INVALIDATE": "0",
            "EASYSTICK_IDMAC_NONCOHERENT_RING": args.idmac_noncoherent_ring,
            "EASYSTICK_CMD53_RX_DESC_BYTES": "0",
            "EASYSTICK_ESPHOSTED_TX_LEDGER": "0",
            "EASYSTICK_TCP22_LEDGER": "0",
            "EASYSTICK_SSH_LEDGER": "0",
            "EASYSTICK_ESPHOSTED_DIAGNOSTICS": "0",
            "EASYSTICK_DW_MMC_CMD53_ERR_PROV": "0",
        },
        "artifacts": {
            "boot_shim_bin": str(boot_bin),
            "boot_shim_bin_sha256": sha256_file(boot_bin),
            "Image": str(image),
            "Image_sha256": image_sha,
            "dtb": str(dtb),
            "dtb_sha256": sha256_file(dtb),
            "rootfs": str(rootfs),
            "rootfs_sha256": sha256_file(rootfs),
            "rendered_0052": str(rendered),
            "rendered_0052_sha256": sha256_file(rendered),
        },
        "flash_rule": (
            "Flash only when shot_c_allowed=true. Re-run "
            "final_shot_manifest.py verify immediately before flash."
        ),
        "experiment": "0053-post-mmc_request_done",
    }
    if args.rendered_0053:
        p53 = Path(args.rendered_0053)
        if not p53.is_file():
            raise SystemExit(f"CMD53_BB_GATE_FAIL: missing artifact {p53}")
        if extract_pa_from_patch(p53) != pa:
            raise SystemExit("CMD53_BB_GATE_FAIL: PA mismatch in rendered 0053")
        if extract_define_u32(p53, "EASYSTICK_CMD53_BB_VERSION") != bb_version:
            raise SystemExit("CMD53_BB_GATE_FAIL: BB version mismatch in rendered 0053")
        doc["artifacts"]["rendered_0053"] = str(p53)
        doc["artifacts"]["rendered_0053_sha256"] = sha256_file(p53)
    if args.rendered_0054:
        p54 = Path(args.rendered_0054)
        if not p54.is_file():
            raise SystemExit(f"CMD53_BB_GATE_FAIL: missing artifact {p54}")
        if extract_pa_from_patch(p54) != pa:
            raise SystemExit("CMD53_BB_GATE_FAIL: PA mismatch in rendered 0054")
        doc["artifacts"]["rendered_0054"] = str(p54)
        doc["artifacts"]["rendered_0054_sha256"] = sha256_file(p54)
    if args.rendered_0026:
        p26 = Path(args.rendered_0026)
        if not p26.is_file():
            raise SystemExit(f"CMD53_BB_GATE_FAIL: missing artifact {p26}")
        if extract_pa_from_patch(p26) != pa:
            raise SystemExit("CMD53_BB_GATE_FAIL: PA mismatch in rendered 0026")
        if extract_define_u32(p26, "EASYSTICK_CMD53_BB_VERSION") != bb_version:
            raise SystemExit("CMD53_BB_GATE_FAIL: BB version mismatch in rendered 0026")
        doc["artifacts"]["rendered_0026_esp_hosted"] = str(p26)
        doc["artifacts"]["rendered_0026_esp_hosted_sha256"] = sha256_file(p26)
    if marker_patch is not None:
        doc["artifacts"]["rendered_0030_cmd52_marker"] = str(marker_patch)
        doc["artifacts"]["rendered_0030_cmd52_marker_sha256"] = sha256_file(
            marker_patch
        )
    out = Path(args.out)
    out.write_text(json.dumps(doc, indent=2) + "\n", encoding="utf-8")
    print(f"CMD53_BB_FINAL_SHOT {out} shot_c_allowed={shot_c_allowed}")
    return 0


def verify_manifest(args: argparse.Namespace) -> int:
    man = json.loads(Path(args.manifest).read_text(encoding="utf-8"))
    if args.require_shot_c and not man.get("shot_c_allowed"):
        raise SystemExit(
            "CMD53_BB_GATE_FAIL: manifest is selftest; rebuild non-selftest final"
        )
    if man.get("easystick_cmd53_bb_version") != EXPECTED_BB_VERSION:
        raise SystemExit("CMD53_BB_GATE_FAIL: unexpected manifest BB version")
    if man.get("easystick_cmd53_bb_size") != EXPECTED_BB_SIZE:
        raise SystemExit("CMD53_BB_GATE_FAIL: unexpected manifest BB size")
    arts = man["artifacts"]
    checks = [
        ("boot_shim_bin", "boot_shim_bin_sha256"),
        ("Image", "Image_sha256"),
        ("dtb", "dtb_sha256"),
        ("rootfs", "rootfs_sha256"),
        ("rendered_0052", "rendered_0052_sha256"),
    ]
    for optional in (
        ("rendered_0053", "rendered_0053_sha256"),
        ("rendered_0054", "rendered_0054_sha256"),
        ("rendered_0026_esp_hosted", "rendered_0026_esp_hosted_sha256"),
    ):
        if optional[0] in arts:
            checks.append(optional)
    for path_key, sha_key in checks:
        path = Path(arts[path_key])
        if args.override_dir:
            # Allow verifying copies under a staging dir by basename.
            alt = Path(args.override_dir) / path.name
            if alt.is_file():
                path = alt
        if not path.is_file():
            raise SystemExit(f"CMD53_BB_GATE_FAIL: missing {path}")
        got = sha256_file(path)
        exp = arts[sha_key]
        if got.lower() != exp.lower():
            raise SystemExit(
                f"CMD53_BB_GATE_FAIL: SHA mismatch {path_key}: got={got} exp={exp}"
            )
    patch = Path(arts["rendered_0052"])
    if args.override_dir:
        alt = Path(args.override_dir) / patch.name
        if alt.is_file():
            patch = alt
    patch_pa = extract_pa_from_patch(patch)
    if patch_pa != man["easystick_cmd53_bb_pa"]:
        raise SystemExit("CMD53_BB_GATE_FAIL: PA vs rendered 0052 mismatch")
    if extract_define_u32(patch, "EASYSTICK_CMD53_BB_VERSION") != man[
        "easystick_cmd53_bb_version"
    ]:
        raise SystemExit("CMD53_BB_GATE_FAIL: manifest vs rendered BB version mismatch")
    marker_enabled = bool(man.get("cmd52_marker_enabled"))
    if marker_enabled:
        if man.get("cmd52_marker_words_count") != EXPECTED_CMD52_MARKER_WORDS:
            raise SystemExit("CMD52_MARKER_GATE_FAIL: manifest marker word count")
        if man.get("cmd52_marker_size") != EXPECTED_CMD52_MARKER_SIZE:
            raise SystemExit("CMD52_MARKER_GATE_FAIL: manifest marker size")
        expected_marker_pa = (
            int(man["easystick_cmd53_bb_pa"].rstrip("u"), 16)
            + EXPECTED_CMD52_MARKER_OFFSET
        )
        if man.get("cmd52_marker_pa") != f"0x{expected_marker_pa:08x}u":
            raise SystemExit("CMD52_MARKER_GATE_FAIL: manifest marker PA mismatch")
        marker_artifact = arts.get("rendered_0030_cmd52_marker")
        marker_sha = arts.get("rendered_0030_cmd52_marker_sha256")
        if not marker_artifact or not marker_sha:
            raise SystemExit("CMD52_MARKER_GATE_FAIL: marker artifact missing")
        marker_path = Path(marker_artifact)
        if args.override_dir:
            alt = Path(args.override_dir) / marker_path.name
            if alt.is_file():
                marker_path = alt
        if not marker_path.is_file():
            raise SystemExit(f"CMD52_MARKER_GATE_FAIL: missing {marker_path}")
        if sha256_file(marker_path).lower() != marker_sha.lower():
            raise SystemExit("CMD52_MARKER_GATE_FAIL: marker SHA mismatch")
        if extract_pa_from_patch(marker_path) != man["easystick_cmd53_bb_pa"]:
            raise SystemExit("CMD52_MARKER_GATE_FAIL: marker PA mismatch")
        if extract_define_u32(
            marker_path, "EASYSTICK_CMD52_MARKER_WORDS"
        ) != EXPECTED_CMD52_MARKER_WORDS:
            raise SystemExit("CMD52_MARKER_GATE_FAIL: marker word count mismatch")
        for name, expected in EXPECTED_CMD52_MARKER_VALUES.items():
            if extract_define_u32(marker_path, name) != expected:
                raise SystemExit(
                    f"CMD52_MARKER_GATE_FAIL: marker value mismatch for {name}"
                )
    print("CMD53_BB_FINAL_SHOT_VERIFY OK")
    return 0


def main() -> int:
    ap = argparse.ArgumentParser()
    sub = ap.add_subparsers(dest="cmd", required=True)

    w = sub.add_parser("write")
    w.add_argument("--out", required=True)
    w.add_argument("--pa", required=True)
    w.add_argument("--product-profile", required=True)
    w.add_argument("--boot-shim-profile", required=True)
    w.add_argument("--boot-shim-bin", required=True)
    w.add_argument("--image", required=True)
    w.add_argument("--dtb", required=True)
    w.add_argument("--rootfs", required=True)
    w.add_argument("--rendered-0052", required=True)
    w.add_argument("--rendered-0053", default="")
    w.add_argument("--rendered-0054", default="")
    w.add_argument("--rendered-0026", default="")
    w.add_argument("--rendered-0030", default="")
    w.add_argument(
        "--cmd52-marker",
        action="store_true",
        help="Record the six-word CMD52 positive-control marker patch",
    )
    w.add_argument("--bb-size", type=lambda value: int(value, 0), required=True)
    w.add_argument(
        "--idmac-noncoherent-ring",
        choices=("0", "1"),
        default="0",
        help="Record the actual IDMAC ring mode used to build this image",
    )
    w.add_argument("--selftest", action="store_true")
    w.add_argument("--selftest-torn", action="store_true")

    v = sub.add_parser("verify")
    v.add_argument("--manifest", required=True)
    v.add_argument("--require-shot-c", action="store_true")
    v.add_argument(
        "--override-dir",
        help="Optional directory of staged flash copies matched by basename",
    )

    args = ap.parse_args()
    if args.cmd == "write":
        return write_manifest(args)
    return verify_manifest(args)


if __name__ == "__main__":
    raise SystemExit(main())
