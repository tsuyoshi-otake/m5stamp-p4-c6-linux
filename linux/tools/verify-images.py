#!/usr/bin/env python3
"""Validate the candidate Stamp-P4 flash map and optional image artifacts.

This tool is deliberately read-only. It never opens a serial port or modifies
an image; it only checks the map before a future flash command consumes it.
"""

from __future__ import annotations

import argparse
import csv
import hashlib
import json
import sys
from pathlib import Path


def number(value: object, field: str) -> int:
    if isinstance(value, bool):
        raise ValueError(f"{field} must be an integer")
    if isinstance(value, int):
        return value
    if isinstance(value, str):
        return int(value, 0)
    raise ValueError(f"{field} must be an integer or 0x-prefixed string")


def fail(message: str) -> "NoReturn":
    print(f"FAIL: {message}", file=sys.stderr)
    raise SystemExit(1)


def parse_artifact(value: str) -> tuple[str, Path]:
    if "=" not in value:
        raise argparse.ArgumentTypeError("artifact must be NAME=PATH")
    name, raw_path = value.split("=", 1)
    if not name or not raw_path:
        raise argparse.ArgumentTypeError("artifact must be NAME=PATH")
    return name, Path(raw_path)


AB_REGIONS = {
    "boot": (0xF40000, 0x40000),
    "boot_alt": (0xF80000, 0x40000),
    "bootmeta0": (0xFC0000, 0x1000),
    "bootmeta1": (0xFC1000, 0x1000),
    "tail-reserve": (0xFC2000, 0x3E000),
}


def validate_boot_ab_layout(regions: list[dict[str, object]]) -> None:
    by_name = {str(region["name"]): region for region in regions}
    for name, (offset, size) in AB_REGIONS.items():
        region = by_name.get(name)
        if region is None:
            fail(f"missing boot A/B region: {name}")
        try:
            actual_offset = number(region["offset"], f"{name}.offset")
            actual_size = number(region["size_bytes"], f"{name}.size_bytes")
        except (KeyError, ValueError) as exc:
            fail(str(exc))
        if (actual_offset, actual_size) != (offset, size):
            fail(f"{name} must be 0x{offset:x}+0x{size:x}, got "
                 f"0x{actual_offset:x}+0x{actual_size:x}")
    if by_name["boot"].get("artifact") is not True:
        fail("boot must remain an externally flashable artifact region")
    for name in ("boot_alt", "bootmeta0", "bootmeta1"):
        if by_name[name].get("artifact") is not False:
            fail(f"{name} must not be an externally flashable artifact region")


def validate_partition_table(table_path: Path,
                             regions: list[dict[str, object]]) -> None:
    try:
        lines = table_path.read_text(encoding="utf-8").splitlines()
    except OSError as exc:
        fail(f"cannot read partition table: {exc}")
    rows: dict[str, tuple[int, int]] = {}
    for line in lines:
        if not line.strip() or line.lstrip().startswith("#"):
            continue
        row = next(csv.reader([line], skipinitialspace=True))
        if len(row) < 5:
            fail(f"invalid partition-table row: {line}")
        name = row[0].strip()
        try:
            offset = int(row[3].strip(), 0)
            size = int(row[4].strip(), 0)
        except ValueError as exc:
            fail(f"invalid offset/size in partition-table row {name!r}: {exc}")
        if name in rows:
            fail(f"duplicate partition-table name: {name}")
        rows[name] = (offset, size)

    layout_names = {
        "factory": "boot-shim",
        "kernel": "kernel",
        "rootfs": "rootfs",
        "dtb": "dtb",
        "crashlog": "crashlog",
        "boot": "boot",
        "boot_alt": "boot_alt",
        "bootmeta0": "bootmeta0",
        "bootmeta1": "bootmeta1",
    }
    by_name = {str(region["name"]): region for region in regions}
    for table_name, layout_name in layout_names.items():
        if table_name not in rows:
            fail(f"partition table is missing {table_name}")
        region = by_name.get(layout_name)
        if region is None:
            fail(f"layout is missing {layout_name}")
        expected = (number(region["offset"], f"{layout_name}.offset"),
                    number(region["size_bytes"], f"{layout_name}.size_bytes"))
        if rows[table_name] != expected:
            fail(f"partition table {table_name} is 0x{rows[table_name][0]:x}+"
                 f"0x{rows[table_name][1]:x}, expected 0x{expected[0]:x}+"
                 f"0x{expected[1]:x}")


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--layout", type=Path, required=True)
    parser.add_argument("--artifact", action="append", default=[], type=parse_artifact,
                        help="optional NAME=PATH check; may be repeated")
    parser.add_argument("--partition-table", type=Path,
                        help="optional ESP-IDF CSV; require it to match the layout")
    parser.add_argument("--require-ready", action="store_true",
                        help="reject candidate_not_for_flash maps")
    parser.add_argument("--json", action="store_true", dest="as_json")
    args = parser.parse_args()

    try:
        layout = json.loads(args.layout.read_text(encoding="utf-8"))
    except (OSError, json.JSONDecodeError) as exc:
        fail(f"cannot read layout: {exc}")
    if args.require_ready and layout.get("status") != "ready_for_flash":
        fail(f"layout status is {layout.get('status')!r}, not ready_for_flash")

    try:
        flash_bytes = number(layout["flash_bytes"], "flash_bytes")
        alignment = number(layout["alignment_bytes"], "alignment_bytes")
    except (KeyError, ValueError) as exc:
        fail(str(exc))
    if flash_bytes <= 0 or alignment <= 0 or alignment & (alignment - 1):
        fail("flash_bytes/alignment_bytes must be positive and alignment a power of two")

    regions = layout.get("regions")
    if not isinstance(regions, list) or not regions:
        fail("layout must contain a non-empty regions list")

    parsed: list[tuple[int, int, dict[str, object]]] = []
    names: set[str] = set()
    for raw in regions:
        if not isinstance(raw, dict):
            fail("each region must be an object")
        name = raw.get("name")
        if not isinstance(name, str) or not name:
            fail("every region needs a non-empty name")
        if name in names:
            fail(f"duplicate region name: {name}")
        names.add(name)
        try:
            offset = number(raw["offset"], f"{name}.offset")
            size = number(raw["size_bytes"], f"{name}.size_bytes")
        except (KeyError, ValueError) as exc:
            fail(str(exc))
        if offset < 0 or size <= 0:
            fail(f"{name} has invalid offset/size")
        if offset % alignment or size % alignment:
            fail(f"{name} is not {alignment}-byte aligned")
        if raw.get("power_of_two") is True and size & (size - 1):
            fail(f"{name} is marked power_of_two but has size {size}")
        end = offset + size
        if end > flash_bytes:
            fail(f"{name} ends at 0x{end:x}, beyond flash size 0x{flash_bytes:x}")
        parsed.append((offset, end, raw))

    parsed.sort(key=lambda item: item[0])
    for previous, current in zip(parsed, parsed[1:]):
        if previous[1] > current[0]:
            fail(f"overlap: {previous[2]['name']} and {current[2]['name']}")

    validate_boot_ab_layout([raw for _, _, raw in parsed])
    if args.partition_table:
        validate_partition_table(args.partition_table, [raw for _, _, raw in parsed])

    artifact_regions = {raw["name"]: raw for raw in regions if raw.get("artifact") is True}
    artifact_checks = []
    for name, path in args.artifact:
        if name not in artifact_regions:
            fail(f"artifact {name!r} has no artifact region")
        if not path.is_file():
            fail(f"artifact file does not exist: {path}")
        size = path.stat().st_size
        limit = number(artifact_regions[name]["size_bytes"], f"{name}.size_bytes")
        if size > limit:
            fail(f"{name} is {size} bytes, larger than its {limit}-byte region")
        artifact_checks.append({"name": name, "bytes": size,
                                "sha256": hashlib.sha256(path.read_bytes()).hexdigest()})

    result = {
        "status": layout.get("status"),
        "flash_bytes": flash_bytes,
        "region_count": len(regions),
        "artifact_checks": artifact_checks,
    }
    if args.as_json:
        print(json.dumps(result, sort_keys=True))
    else:
        print("PASS: candidate flash map has no overlaps and fits measured 16 MiB flash")
        print(f"       status={layout.get('status')}, regions={len(regions)}, artifact_checks={len(artifact_checks)}")
    return 0


if __name__ == "__main__":
    main()
