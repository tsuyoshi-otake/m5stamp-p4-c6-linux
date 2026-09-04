#!/usr/bin/env python3
"""Fail if common credentials are accidentally added to the firmware tree."""

from __future__ import annotations

import argparse
import re
import sys
from pathlib import Path


PATTERNS = (
    re.compile(r"-----BEGIN (?:OPENSSH|RSA|EC|DSA|PRIVATE) KEY-----"),
    re.compile(r"\b(?:ghp|github_pat|xox[baprs])-[A-Za-z0-9-]{12,}"),
    re.compile(r"(?im)^\s*(?:WIFI_PASSWORD|WIFI_PSK|SSH_PRIVATE_KEY)\s*[:=]\s*\S+"),
)
SKIP_NAMES = {"README.md", "versions.lock.json", "board-contract.json"}


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--root", type=Path, default=Path(__file__).resolve().parents[1])
    args = parser.parse_args()

    findings: list[str] = []
    for path in args.root.rglob("*"):
        if not path.is_file() or path.name in SKIP_NAMES:
            continue
        try:
            text = path.read_text(encoding="utf-8")
        except (UnicodeDecodeError, OSError):
            continue
        for pattern in PATTERNS:
            if pattern.search(text):
                findings.append(str(path))
                break

    if findings:
        print("credential-like material found in:")
        print("\n".join(f"  {path}" for path in findings))
        return 1
    print("PASS: no credential-like material found")
    return 0


if __name__ == "__main__":
    sys.exit(main())
