#!/usr/bin/env python3
"""Verify the ordered transport/network markers in a raw P1 UART capture."""

from __future__ import annotations

import argparse
import hashlib
import json
import re
import sys
from pathlib import Path


MARKERS = (
    ("01", "C6_RESET", re.compile(r"EASYSTICK_P1 STEP 01 C6_RESET PASS")),
    ("02", "SDIO_ENUMERATE", re.compile(r"EASYSTICK_P1 STEP 02 SDIO_ENUMERATE PASS")),
    ("03", "FUNC1_ENABLE", re.compile(r"EASYSTICK_P1 STEP 03 FUNC1_ENABLE PASS")),
    ("04", "CMD52", re.compile(r"EASYSTICK_P1 STEP 04 CMD52 PASS")),
    ("05", "CMD53_SMALL", re.compile(r"EASYSTICK_P1 STEP 05 CMD53_SMALL PASS")),
    ("06", "CMD53_512", re.compile(r"EASYSTICK_P1 STEP 06 CMD53_512 PASS")),
    ("07", "CMD53_REPEAT", re.compile(r"EASYSTICK_P1 STEP 07 CMD53_REPEAT PASS")),
    ("08", "ESP_HOSTED_INIT", re.compile(r"EASYSTICK_P1 STEP 08 ESP_HOSTED_INIT PASS")),
    ("09", "WIFI_SCAN", re.compile(r"EASYSTICK_P1 STEP 09 WIFI_SCAN PASS")),
    ("10", "ASSOCIATION", re.compile(r"EASYSTICK_P1 STEP 10 ASSOCIATION PASS")),
    ("11", "PING", re.compile(r"EASYSTICK_P1 STEP 11 PING PASS")),
)


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("capture", type=Path)
    parser.add_argument("--report", type=Path)
    args = parser.parse_args()

    raw = args.capture.read_bytes()
    text = raw.decode("utf-8", errors="replace")
    failures: list[str] = []
    ordered: list[dict[str, object]] = []
    previous = -1

    for number, name, pattern in MARKERS:
        match = pattern.search(text, previous + 1)
        if match is None:
            failures.append(f"missing ordered PASS marker: STEP {number} {name}")
            continue
        line_start = text.rfind("\n", 0, match.start()) + 1
        line_end = text.find("\n", match.end())
        if line_end < 0:
            line_end = len(text)
        ordered.append(
            {
                "step": int(number),
                "name": name,
                "offset": match.start(),
                "line": text[line_start:line_end],
            }
        )
        previous = match.start()

    error_matches = re.findall(r"EASYSTICK_P1 [^\r\n]*\bFAIL\b", text)
    if error_matches:
        failures.append(f"unclassified P1 failure markers present: {len(error_matches)}")

    report = {
        "schema": 1,
        "capture": str(args.capture),
        "bytes": len(raw),
        "sha256": hashlib.sha256(raw).hexdigest(),
        "ordered_pass_markers": ordered,
        "failure_marker_count": len(error_matches),
        "p1_status": "FAIL" if failures else "PASS",
        "failures": failures,
    }
    rendered = json.dumps(report, indent=2, ensure_ascii=False) + "\n"
    if args.report:
        if args.report.exists():
            raise SystemExit(f"refusing to overwrite capture report: {args.report}")
        args.report.write_text(rendered, encoding="utf-8")
    print(rendered, end="")
    if failures:
        return 1
    print("PASS: all 11 P1 markers are present in order and no failure marker was found")
    return 0


if __name__ == "__main__":
    sys.exit(main())
