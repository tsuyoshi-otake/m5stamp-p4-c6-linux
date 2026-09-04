#!/usr/bin/env python3
"""Emit one C68 source-to-artifact manifest. Fail-closed on missing inputs."""
from pathlib import Path
import hashlib
import json
import os
import sys


def sha256_file(path: Path) -> str:
    h = hashlib.sha256()
    with path.open("rb") as f:
        for chunk in iter(lambda: f.read(1 << 20), b""):
            h.update(chunk)
    return h.hexdigest()


def main() -> int:
    out = Path(sys.argv[1])
    payload = json.loads(Path(sys.argv[2]).read_text(encoding="utf-8"))
    files = payload["files"]
    for key, path_s in files.items():
        p = Path(path_s)
        if not p.is_file():
            print(f"C68_MANIFEST_FAIL: missing {key}={p}", file=sys.stderr)
            return 1
        payload.setdefault("sha256", {})[key] = sha256_file(p)
    payload["environ"] = {
        "EASYSTICK_C68_CLEAN_RELEASE": os.environ.get("EASYSTICK_C68_CLEAN_RELEASE", ""),
        "EASYSTICK_KERNEL_PATCH_OVERRIDE_DIR": os.environ.get(
            "EASYSTICK_KERNEL_PATCH_OVERRIDE_DIR", ""
        ),
    }
    out.write_text(json.dumps(payload, indent=2, sort_keys=True) + "\n", encoding="utf-8")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
