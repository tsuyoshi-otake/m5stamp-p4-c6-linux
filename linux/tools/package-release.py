#!/usr/bin/env python3
"""Build deterministic partition and 16 MiB release archives."""

from __future__ import annotations

import argparse
import gzip
import hashlib
import re
import stat
import subprocess
import sys
import zipfile
from pathlib import Path


FLASH_BYTES = 0x1000000
ARTIFACTS = {
    "bootloader.bin": (0x002000, 0x006000, "{shim}/build/bootloader/bootloader.bin"),
    "partition-table.bin": (0x008000, 0x001000, "{shim}/build/partition_table/partition-table.bin"),
    "boot-shim.bin": (0x010000, 0x080000, "{shim}/build/easystick_stamp_p4_boot_shim.bin"),
    "Image": (0x090000, 0x780000, "buildroot/images/Image"),
    "rootfs.squashfs": (0x810000, 0x700000, "buildroot/images/rootfs.squashfs"),
    "easystick-stamp-p4.dtb": (0xF10000, 0x010000, "buildroot/images/easystick-stamp-p4.dtb"),
}
BOOT_OFFSET = 0xF40000
BOOT_BYTES = 0x40000


def sha256(path: Path) -> str:
    digest = hashlib.sha256()
    with path.open("rb") as stream:
        for block in iter(lambda: stream.read(1024 * 1024), b""):
            digest.update(block)
    return digest.hexdigest()


def checked_file(path: Path, maximum: int, *, exact: bool = False) -> bytes:
    try:
        data = path.read_bytes()
    except OSError as exc:
        raise SystemExit(f"cannot read release artifact {path}: {exc}") from exc
    if (exact and len(data) != maximum) or (not exact and not 0 < len(data) <= maximum):
        expectation = "exactly" if exact else "1.."
        raise SystemExit(
            f"invalid artifact size for {path}: {len(data)}; expected {expectation}{maximum}"
        )
    return data


def zip_member(archive: zipfile.ZipFile, name: str, data: bytes, mode: int = 0o644) -> None:
    info = zipfile.ZipInfo(name, (1980, 1, 1, 0, 0, 0))
    info.compress_type = zipfile.ZIP_DEFLATED
    info.external_attr = (stat.S_IFREG | mode) << 16
    archive.writestr(info, data)


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--build-output", type=Path, required=True)
    parser.add_argument("--boot-shim-directory", default="boot-shim-c68")
    parser.add_argument("--boot-image", type=Path, required=True)
    parser.add_argument("--output", type=Path, required=True)
    parser.add_argument("--version", required=True, help="release version such as v0.2.1")
    args = parser.parse_args()

    if not re.fullmatch(r"v[0-9]+\.[0-9]+\.[0-9]+", args.version):
        raise SystemExit("--version must have the form vMAJOR.MINOR.PATCH")

    repo_root = Path(__file__).resolve().parents[2]
    verifier = Path(__file__).with_name("verify-images.py")
    layout = repo_root / "linux" / "m2" / "flash-layout.json"
    partitions = repo_root / "linux" / "boot-shim" / "partitions-m2.csv"
    subprocess.run(
        [sys.executable, str(verifier), "--layout", str(layout),
         "--partition-table", str(partitions), "--require-ready", "--json"],
        check=True,
        stdout=subprocess.DEVNULL,
    )

    args.output.mkdir(parents=True, exist_ok=True)
    prefix = f"m5stamp-p4-c6-linux-smp-{args.version}"
    raw_path = args.output / f"{prefix}-16mb.bin"
    gzip_path = args.output / f"{prefix}-16mb.bin.gz"
    zip_path = args.output / f"{prefix}.zip"

    members: dict[str, bytes] = {}
    flash = bytearray(b"\xff") * FLASH_BYTES
    for name, (offset, maximum, relative) in ARTIFACTS.items():
        source = args.build_output / relative.format(shim=args.boot_shim_directory)
        data = checked_file(source, maximum)
        members[name] = data
        flash[offset:offset + len(data)] = data

    boot = checked_file(args.boot_image, BOOT_BYTES, exact=True)
    if boot[510:512] != b"\x55\xaa":
        raise SystemExit(f"boot image has no DOS boot signature: {args.boot_image}")
    members["boot.img"] = boot
    flash[BOOT_OFFSET:BOOT_OFFSET + BOOT_BYTES] = boot

    raw_path.write_bytes(flash)
    gzip_path.write_bytes(gzip.compress(flash, compresslevel=9, mtime=0))

    member_hashes = "".join(
        f"{hashlib.sha256(data).hexdigest()}  {name}\n"
        for name, data in sorted(members.items())
    ).encode("ascii")
    flash_script = (repo_root / "linux" / "flash-candidate.ps1").read_bytes()
    layout_data = layout.read_bytes()
    with zipfile.ZipFile(zip_path, "w") as archive:
        for name, data in sorted(members.items()):
            zip_member(archive, name, data)
        zip_member(archive, "flash-candidate.ps1", flash_script)
        zip_member(archive, "flash-layout.json", layout_data)
        zip_member(archive, "SHA256SUMS.txt", member_hashes)

    release_hashes = args.output / "SHA256SUMS.txt"
    release_hashes.write_text(
        f"{sha256(gzip_path)}  {gzip_path.name}\n"
        f"{sha256(raw_path)}  {raw_path.name}\n"
        f"{sha256(zip_path)}  {zip_path.name}\n",
        encoding="ascii",
        newline="\n",
    )
    print(f"release package: {zip_path}")
    print(f"monolithic image: {gzip_path}")
    print(f"checksums: {release_hashes}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
