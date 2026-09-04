#!/usr/bin/env python3
"""Render 0052 from template by substituting boot-shim nm PA."""
from pathlib import Path
import sys

PLACEHOLDER = "0xBBDEAD01u"


def main() -> int:
    src = Path(sys.argv[1]).resolve()
    dest = Path(sys.argv[2]).resolve()
    bb_pa = sys.argv[3]
    if not bb_pa.startswith("0x") or not bb_pa.endswith("u"):
        print("CMD53_BB_GATE_FAIL: PA must look like 0x........u", file=sys.stderr)
        return 1
    if dest.parent == src.parent and dest.name == src.name:
        print("CMD53_BB_GATE_FAIL: refuse overwrite of template", file=sys.stderr)
        return 1
    text = src.read_text(encoding="utf-8")
    if PLACEHOLDER not in text:
        print("CMD53_BB_GATE_FAIL: template missing placeholder", file=sys.stderr)
        return 1
    rendered = text.replace(PLACEHOLDER, bb_pa)
    if "BBDEAD" in rendered:
        print("CMD53_BB_GATE_FAIL: placeholder remains", file=sys.stderr)
        return 1
    if bb_pa not in rendered:
        print("CMD53_BB_GATE_FAIL: nm PA missing from rendered patch", file=sys.stderr)
        return 1
    dest.parent.mkdir(parents=True, exist_ok=True)
    dest.write_text(rendered, encoding="utf-8", newline="\n")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
