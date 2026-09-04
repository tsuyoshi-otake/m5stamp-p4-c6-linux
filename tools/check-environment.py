#!/usr/bin/env python3
"""Report the Linux host prerequisites for the native P4 build.

The checker is intentionally non-installing. It is safe to run on Windows to
see what must be provided by WSL2, a VM, or a Linux container.
"""

from __future__ import annotations

import argparse
import json
import shutil
import subprocess
import sys
from typing import Optional


REQUIRED = (
    "git",
    "make",
    "gcc",
    "g++",
    "bison",
    "flex",
    "bc",
    "perl",
    "python3",
    "patch",
    "cpio",
    "rsync",
    "unzip",
    "wget",
    "file",
    "gzip",
    "bzip2",
    "tar",
)
RECOMMENDED = ("cmake", "ninja", "dtc", "openssl")


def usable(name: str) -> Optional[str]:
    path = shutil.which(name)
    if path is None:
        return None
    try:
        subprocess.run((path, "--version"), stdout=subprocess.DEVNULL,
                       stderr=subprocess.DEVNULL, check=True, timeout=3)
    except (OSError, subprocess.SubprocessError):
        return None
    return path


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--json", action="store_true", help="emit machine-readable output")
    parser.add_argument("--allow-missing", action="store_true", help="report only; always exit zero")
    args = parser.parse_args()

    result = {
        "required": {name: usable(name) for name in REQUIRED},
        "recommended": {name: usable(name) for name in RECOMMENDED},
    }
    missing = [name for name, path in result["required"].items() if path is None]

    if args.json:
        print(json.dumps({**result, "missing_required": missing}, indent=2))
    else:
        for group in ("required", "recommended"):
            print(group + ":")
            for name, path in result[group].items():
                print(f"  {'OK ' if path else 'MISS'} {name}{' -> ' + path if path else ''}")
        if missing:
            print("missing required tools: " + ", ".join(missing))

    return 0 if args.allow_missing or not missing else 1


if __name__ == "__main__":
    sys.exit(main())
