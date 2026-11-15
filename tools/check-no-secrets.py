#!/usr/bin/env python3
"""Fail if common credentials are accidentally added to the firmware tree."""

from __future__ import annotations

import argparse
import re
import subprocess
import sys
from pathlib import Path


PATTERNS = (
    re.compile(r"-----BEGIN (?:OPENSSH|RSA|EC|DSA|PRIVATE) KEY-----"),
    re.compile(r"\b(?:ghp|github_pat|xox[baprs])-[A-Za-z0-9-]{12,}"),
)
KEY_VALUE = re.compile(
    r"(?im)^\s*(?:WIFI_PASSWORD|WIFI_PSK|SSH_PRIVATE_KEY)\s*[:=]\s*(\S+)")
SKIP_NAMES = {"README.md", "versions.lock.json", "board-contract.json",
              "board-contract-stamp-p4.json", "board-contract-tab5.json"}
# Test fixtures carry obviously fake credentials; they are tolerated only
# under the directories in DUMMY_DIRS.
DUMMY_VALUES = {"secret123"}
DUMMY_DIRS = ("linux/tests/",)


def candidates(root: Path) -> list[Path]:
    """Files to scan: git's tracked set, or the whole tree when git is absent."""
    try:
        out = subprocess.run(
            ["git", "-C", str(root), "ls-files", "-z"],
            check=True, capture_output=True).stdout
    except (OSError, subprocess.CalledProcessError):
        return [path for path in root.rglob("*") if path.is_file()]
    return [root / name.decode("utf-8", "surrogateescape")
            for name in out.split(b"\0") if name]


def flagged(rel: str, text: str) -> bool:
    if any(pattern.search(text) for pattern in PATTERNS):
        return True
    dummy_ok = rel.startswith(DUMMY_DIRS)
    return any(not dummy_ok or match.group(1) not in DUMMY_VALUES
               for match in KEY_VALUE.finditer(text))


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--root", type=Path, default=Path(__file__).resolve().parents[1])
    args = parser.parse_args(argv)

    findings: list[str] = []
    for path in candidates(args.root):
        if not path.is_file() or path.name in SKIP_NAMES:
            continue
        try:
            rel = path.relative_to(args.root).as_posix()
            text = path.read_text(encoding="utf-8")
        except (UnicodeDecodeError, OSError, ValueError):
            continue
        if flagged(rel, text):
            findings.append(str(path))

    if findings:
        print("credential-like material found in:")
        print("\n".join(f"  {path}" for path in findings))
        return 1
    print("PASS: no credential-like material found")
    return 0


if __name__ == "__main__":
    sys.exit(main())
