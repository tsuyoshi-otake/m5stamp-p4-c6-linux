#!/usr/bin/env python3
"""Extract ESP app images from a raw C6 flash backup and verify the old NG hash."""

from __future__ import annotations

import argparse
import hashlib
import json
import struct
import sys
from pathlib import Path


PARTITION_TABLE_OFFSET = 0x8000
PARTITION_ENTRY_SIZE = 32
PARTITION_TABLE_MAGIC = 0x50AA
APP_PARTITION_TYPE = 0
IMAGE_MAGIC = 0xE9
IMAGE_HEADER_SIZE = 24
MAX_SEGMENTS = 16
HASH_SIZE = 32
EXPECTED_NG_SHA256 = (
    "2ac39933c84c7688fb5d251ffc01bd15e53b665a272b6053dedca448e40e1827"
)


def image_length(flash: bytes, offset: int, partition_size: int) -> int:
    image = flash[offset : offset + partition_size]
    if len(image) < IMAGE_HEADER_SIZE or image[0] != IMAGE_MAGIC:
        raise ValueError("ESP image header is missing")
    segment_count = image[1]
    if segment_count > MAX_SEGMENTS:
        raise ValueError(f"invalid segment count {segment_count}")

    cursor = IMAGE_HEADER_SIZE
    for _ in range(segment_count):
        if cursor + 8 > len(image):
            raise ValueError("segment header exceeds partition")
        _load_address, data_length = struct.unpack_from("<II", image, cursor)
        cursor += 8
        if data_length > len(image) - cursor:
            raise ValueError("segment data exceeds partition")
        cursor += data_length

    # The ROM checksum always follows the last segment.  ESP-IDF images with
    # hash_appended=1 carry a 32-byte simple hash after that checksum.
    cursor += 1
    if image[23] != 0:
        cursor += HASH_SIZE
    if cursor > len(image):
        raise ValueError("image trailer exceeds partition")
    return cursor


def app_partitions(flash: bytes) -> list[dict[str, object]]:
    results: list[dict[str, object]] = []
    table = flash[PARTITION_TABLE_OFFSET : PARTITION_TABLE_OFFSET + 0x1000]
    for index in range(0, len(table), PARTITION_ENTRY_SIZE):
        entry = table[index : index + PARTITION_ENTRY_SIZE]
        if len(entry) < PARTITION_ENTRY_SIZE:
            break
        magic, = struct.unpack_from("<H", entry, 0)
        if magic != PARTITION_TABLE_MAGIC:
            continue
        partition_type = entry[2]
        subtype = entry[3]
        offset, size = struct.unpack_from("<II", entry, 4)
        label = entry[12:28].split(b"\0", 1)[0].decode("ascii", "replace")
        if partition_type != APP_PARTITION_TYPE:
            continue
        record: dict[str, object] = {
            "entry_index": index // PARTITION_ENTRY_SIZE,
            "label": label,
            "subtype": subtype,
            "offset": offset,
            "partition_size": size,
        }
        try:
            length = image_length(flash, offset, size)
            blob = flash[offset : offset + length]
            record["image_bytes"] = length
            record["sha256"] = hashlib.sha256(blob).hexdigest()
            record["expected_ng_match"] = record["sha256"] == EXPECTED_NG_SHA256
        except (IndexError, ValueError) as exc:
            record["error"] = str(exc)
        results.append(record)
    return results


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("flash", type=Path)
    parser.add_argument("--report", type=Path)
    parser.add_argument("--expected-sha256", default=EXPECTED_NG_SHA256)
    args = parser.parse_args()

    flash = args.flash.read_bytes()
    records = app_partitions(flash)
    expected_matches = [
        item for item in records if item.get("sha256") == args.expected_sha256
    ]
    report = {
        "schema": 1,
        "flash_file": str(args.flash),
        "flash_bytes": len(flash),
        "partition_table_offset": PARTITION_TABLE_OFFSET,
        "expected_sha256": args.expected_sha256,
        "app_partitions": records,
        "expected_match_count": len(expected_matches),
    }
    rendered = json.dumps(report, indent=2, ensure_ascii=False) + "\n"
    if args.report:
        args.report.write_text(rendered, encoding="utf-8")
    print(rendered, end="")
    if len(expected_matches) != 1:
        print(
            "FAIL: expected exactly one application partition matching the "
            f"historical NG SHA-256; found {len(expected_matches)}",
            file=sys.stderr,
        )
        return 1
    print(
        "PASS: the raw C6 backup contains exactly one application image "
        f"matching {args.expected_sha256}"
    )
    return 0


if __name__ == "__main__":
    sys.exit(main())
