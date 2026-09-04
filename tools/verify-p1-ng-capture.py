#!/usr/bin/env python3
"""Verify the bounded P1 negative-control boundary with the C6 left unchanged."""

from __future__ import annotations

import argparse
import hashlib
import json
import re
import sys
from pathlib import Path


STEPS = {
    1: "C6_RESET",
    2: "SDIO_ENUMERATE",
    3: "FUNC1_ENABLE",
    4: "CMD52",
    5: "CMD53_SMALL",
    6: "CMD53_512",
    7: "CMD53_REPEAT",
    8: "ESP_HOSTED_INIT",
}
STEP_MARKER = re.compile(
    r"EASYSTICK_P1 STEP (?P<number>0[1-8]) "
    r"(?P<name>[A-Z0-9_]+) (?P<state>PASS|FAIL)\b"
)
ANY_FAILURE = re.compile(r"EASYSTICK_P1 [^\r\n]*\bFAIL\b")


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("capture", type=Path)
    parser.add_argument("--report", type=Path)
    parser.add_argument(
        "--expected-c6-application-sha256",
        default="2ac39933c84c7688fb5d251ffc01bd15e53b665a272b6053dedca448e40e1827",
    )
    args = parser.parse_args()

    raw = args.capture.read_bytes()
    text = raw.decode("utf-8", errors="replace")
    markers = [
        (
            int(match.group("number")),
            match.group("name"),
            match.group("state"),
            match.start(),
            text[text.rfind("\n", 0, match.start()) + 1 : text.find("\n", match.end())]
            if text.find("\n", match.end()) >= 0
            else text[text.rfind("\n", 0, match.start()) + 1 :],
        )
        for match in STEP_MARKER.finditer(text)
    ]
    failures: list[str] = []
    first_failure_index = next(
        (index for index, marker in enumerate(markers) if marker[2] == "FAIL"),
        None,
    )

    if first_failure_index is None:
        failures.append("no STEP 01..08 failure boundary was observed")
        first_failure_step = None
        first_failure_line = None
    else:
        first_failure_step, first_failure_name, _, _, first_failure_line = markers[
            first_failure_index
        ]
        if first_failure_step < 2:
            failures.append(
                "negative control must complete STEP 01 before its first failure"
            )
        if first_failure_name != STEPS[first_failure_step]:
            failures.append(
                f"STEP {first_failure_step:02d} name is {first_failure_name}, "
                f"expected {STEPS[first_failure_step]}"
            )

        prefix = markers[:first_failure_index]
        for expected_step in range(1, first_failure_step):
            matching = [
                marker
                for marker in prefix
                if marker[0] == expected_step and marker[1] == STEPS[expected_step]
            ]
            if not matching:
                failures.append(
                    f"missing ordered PASS marker before failure: "
                    f"STEP {expected_step:02d} {STEPS[expected_step]}"
                )
            elif matching[-1][2] != "PASS":
                failures.append(
                    f"STEP {expected_step:02d} {STEPS[expected_step]} "
                    "did not PASS before the failure boundary"
                )

        later_passes = [
            marker
            for marker in markers[first_failure_index + 1 :]
            if marker[2] == "PASS" and marker[0] >= first_failure_step
        ]
        if later_passes:
            failures.append(
                "a later PASS marker appears after the negative-control failure"
            )

    all_failures = ANY_FAILURE.findall(text)
    expected_boundary = (
        first_failure_line is not None
        and f"STEP {first_failure_step:02d} {STEPS[first_failure_step]} FAIL"
        in first_failure_line
    )
    status = "NEGATIVE_CONTROL_OBSERVED" if not failures and expected_boundary else "FAIL"
    classification = (
        "PROTOCOL_MISMATCH"
        if status == "NEGATIVE_CONTROL_OBSERVED"
        and first_failure_step == 8
        and first_failure_line is not None
        and "got=" in first_failure_line
        and "expected=" in first_failure_line
        else "EARLY_FAILURE_BOUNDARY"
    )
    report = {
        "schema": 1,
        "capture": str(args.capture),
        "bytes": len(raw),
        "sha256": hashlib.sha256(raw).hexdigest(),
        "expected_c6_application_sha256": args.expected_c6_application_sha256,
        "c6_write_performed": False,
        "ordered_step_markers": [
            {
                "step": number,
                "name": name,
                "state": state,
                "offset": offset,
                "line": line.rstrip("\r\n"),
            }
            for number, name, state, offset, line in markers
        ],
        "first_failure_step": first_failure_step,
        "first_failure_line": first_failure_line.rstrip("\r\n")
        if first_failure_line is not None
        else None,
        "failure_marker_count": len(all_failures),
        "negative_control_classification": classification,
        "negative_control_status": status,
        "p1_status": "NOT_APPLICABLE",
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
    print(
        "PASS: P1-NG negative-control boundary observed; "
        "this is not full P1 acceptance"
    )
    return 0


if __name__ == "__main__":
    sys.exit(main())
