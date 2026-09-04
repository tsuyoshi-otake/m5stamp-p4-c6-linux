#!/usr/bin/env python3
"""Validate the candidate Stamp-P4 flash map and optional image artifacts.

This tool is deliberately read-only. It never opens a serial port or modifies
an image; it only checks the map before a future flash command consumes it.
"""

from __future__ import annotations

import argparse
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


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--layout", type=Path, required=True)
    parser.add_argument("--artifact", action="append", default=[], type=parse_artifact,
                        help="optional NAME=PATH check; may be repeated")
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
