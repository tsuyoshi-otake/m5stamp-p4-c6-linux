#!/usr/bin/env python3
"""Build deterministic partition and 16 MiB release archives."""

from __future__ import annotations

import argparse
import gzip
import hashlib
import json
import re
import tarfile
import stat
import subprocess
import sys
import zipfile
from pathlib import Path


FLASH_BYTES = 0x1000000
ARTIFACTS = {
    "bootloader.bin": (0x002000, 0x006000, "{shim}/build/bootloader/bootloader.bin"),
    "partition-table.bin": (0x008000, 0x001000, "{shim}/build/partition_table/partition-table.bin"),
    "boot-shim.bin": (0x010000, 0x080000, "{shim}/build/stamp_p4_boot_shim.bin"),
    "Image": (0x090000, 0x780000, "buildroot/images/Image"),
    "rootfs.squashfs": (0x810000, 0x700000, "buildroot/images/rootfs.squashfs"),
    "esp32p4-m5stamp-p4.dtb": (0xF10000, 0x010000, "buildroot/images/esp32p4-m5stamp-p4.dtb"),
}
# Debug material (#22): needed to resolve crash-capsule epc/ra of the exact image.
DEBUG_FILES = {
    "System.map": "buildroot/build/linux-custom/System.map",
    "vmlinux": "buildroot/build/linux-custom/vmlinux",
    "kernel.config": "buildroot/build/linux-custom/.config",
    "boot-shim.elf": "{shim}/build/stamp_p4_boot_shim.elf",
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
    parser.add_argument("--boot-shim-directory")
    parser.add_argument("--boot-image", type=Path)
    parser.add_argument("--board", choices=("stamp-p4", "waveshare-wifi6"), default="stamp-p4")
    parser.add_argument("--profile", choices=("m1", "m2"), default="m2")
    parser.add_argument("--allow-candidate", action="store_true",
                        help="package a development candidate without asserting release readiness")
    parser.add_argument("--c6-build-output", type=Path,
                        help="paired C6 build output (required for Waveshare m2)")
    parser.add_argument("--output", type=Path, required=True)
    parser.add_argument("--version", required=True, help="release version such as v0.2.1")
    args = parser.parse_args()

    if not re.fullmatch(r"v[0-9]+\.[0-9]+\.[0-9]+", args.version):
        raise SystemExit("--version must have the form vMAJOR.MINOR.PATCH")

    repo_root = Path(__file__).resolve().parents[2]
    verifier = Path(__file__).with_name("verify-images.py")
    layout = repo_root / "linux" / "m2" / "flash-layout.json"
    waveshare = args.board == "waveshare-wifi6"
    if not args.boot_shim_directory:
        args.boot_shim_directory = "boot-shim" if waveshare else "boot-shim-c68"
    artifacts = dict(ARTIFACTS)
    if waveshare:
        if args.boot_image:
            parser.error("keep private Wi-Fi boot images separate from distributable Waveshare archives")
        if args.profile == "m2" and not args.c6_build_output:
            parser.error("Waveshare m2 requires --c6-build-output for its paired firmware")
        layout = repo_root / "linux" / "boards" / "waveshare-wifi6" / "flash-layout.json"
        dtb_name = "esp32p4-waveshare-wifi6" + ("-sdio" if args.profile == "m2" else "") + ".dtb"
        del artifacts["esp32p4-m5stamp-p4.dtb"]
        artifacts[dtb_name] = (0xF10000, 0x10000, "buildroot/images/" + dtb_name)
    elif not args.boot_image:
        parser.error("Stamp packaging requires --boot-image")
    partitions = repo_root / "linux" / "boot-shim" / "partitions-m2.csv"
    verify_args = [sys.executable, str(verifier), "--layout", str(layout),
                   "--partition-table", str(partitions), "--json"]
    if not args.allow_candidate:
        verify_args.append("--require-ready")
    subprocess.run(
        verify_args,
        check=True,
        stdout=subprocess.DEVNULL,
    )

    args.output.mkdir(parents=True, exist_ok=True)
    prefix = (f"waveshare-p4-wifi6-{args.profile}-{args.version}" if waveshare
              else f"m5stamp-p4-c6-linux-smp-{args.version}")
    raw_path = args.output / f"{prefix}-16mb.bin"
    gzip_path = args.output / f"{prefix}-16mb.bin.gz"
    zip_path = args.output / f"{prefix}.zip"

    members: dict[str, bytes] = {}
    # Waveshare uses partition writes so an archive cannot erase NVS or
    # private configuration through a blanket monolithic-image flash.
    flash = None if waveshare else bytearray(b"\xff") * FLASH_BYTES
    for name, (offset, maximum, relative) in artifacts.items():
        source = args.build_output / relative.format(shim=args.boot_shim_directory)
        data = checked_file(source, maximum)
        members[name] = data
        if flash is not None:
            flash[offset:offset + len(data)] = data

    if flash is not None:
        boot = checked_file(args.boot_image, BOOT_BYTES, exact=True)
        if boot[510:512] != b"\x55\xaa":
            raise SystemExit(f"boot image has no DOS boot signature: {args.boot_image}")
        members["boot.img"] = boot
        flash[BOOT_OFFSET:BOOT_OFFSET + BOOT_BYTES] = boot
        raw_path.write_bytes(flash)
        gzip_path.write_bytes(gzip.compress(flash, compresslevel=9, mtime=0))
    if waveshare and args.profile == "m2":
        c6 = args.c6_build_output / "build"
        for name, maximum in (("bootloader/bootloader.bin", 0x8000),
                              ("partition_table/partition-table.bin", 0x1000),
                              ("ota_data_initial.bin", 0x2000),
                              ("network_adapter.bin", 0x120000),
                              ("flasher_args.json", 0x10000)):
            members["c6/" + name] = checked_file(c6 / name, maximum)
        members["c6/C6_PATCH_MANIFEST.txt"] = checked_file(
            args.c6_build_output / "C6_PATCH_MANIFEST.txt", 0x10000)
    if waveshare:
        packaging_commit = subprocess.check_output(
            ["git", "-C", str(repo_root), "rev-parse", "HEAD"], text=True).strip()
        packaging_dirty = bool(subprocess.check_output(
            ["git", "-C", str(repo_root), "status", "--porcelain", "--untracked-files=no"], text=True))
        members["make-wifi-boot.py"] = (repo_root / "tools/make-wifi-boot.py").read_bytes()
        members["build-manifest.json"] = (json.dumps({
            "board": args.board, "profile": args.profile, "version": args.version,
            "status": "development_candidate" if args.allow_candidate else "release",
            "packaging_source_commit": packaging_commit,
            "packaging_source_dirty": packaging_dirty,
            "artifact_identity": "hashes identify tested bytes; embedded build labels may predate packaging",
            "source_lock": json.loads((repo_root / "versions.lock.json").read_text()),
            "p4_offsets": {name: hex(value[0]) for name, value in artifacts.items()},
            "private_configuration": "provision separately; excluded from this archive",
            "hashes": {name: hashlib.sha256(data).hexdigest() for name, data in sorted(members.items())},
        }, indent=2) + "\n").encode()

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

    debug_path = args.output / f"{prefix}-debug.tar.xz"
    with tarfile.open(debug_path, "w:xz") as bundle:
        for name, relative in DEBUG_FILES.items():
            source = args.build_output / relative.format(shim=args.boot_shim_directory)
            if not source.is_file():
                raise SystemExit(f"missing debug artifact {source}")
            info = bundle.gettarinfo(str(source), arcname=name)
            info.uid = info.gid = 0
            info.uname = info.gname = ""
            info.mtime = 0
            with source.open("rb") as stream:
                bundle.addfile(info, stream)

    release_hashes = args.output / "SHA256SUMS.txt"
    release_files = [zip_path, debug_path] if waveshare else [gzip_path, raw_path, zip_path, debug_path]
    release_hashes.write_text(
        "".join(f"{sha256(path)}  {path.name}\n" for path in release_files),
        encoding="ascii",
        newline="\n",
    )
    print(f"release package: {zip_path}")
    if not waveshare:
        print(f"monolithic image: {gzip_path}")
    print(f"checksums: {release_hashes}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
