#!/usr/bin/env python3
"""Copy C68 patches into a generated directory and substitute nm PAs.

The destination must be physically separate from the kernel patch stage.
Source templates keep 0xC68DEAD1/2; rendered copies must not.
"""
from pathlib import Path
import sys

PATCHES = (
    "0038-easystick-c68-clean-release-spinwait.patch",
    "0039-easystick-c68-clean-release-smpboot.patch",
)


def main() -> int:
    src_dir = Path(sys.argv[1]).resolve()
    dest_dir = Path(sys.argv[2]).resolve()
    release_pa = sys.argv[3]
    stage_pa = sys.argv[4]
    if dest_dir == src_dir:
        print("C68_GATE_FAIL: rendered patch dir equals source patch dir", file=sys.stderr)
        return 1
    dest_dir.mkdir(parents=True, exist_ok=True)
    for name in PATCHES:
        src = src_dir / name
        text = src.read_text(encoding="utf-8")
        if "0xC68DEAD1u" not in text or "0xC68DEAD2u" not in text:
            print(f"C68_GATE_FAIL: {name} is missing placeholders", file=sys.stderr)
            return 1
        rendered = text.replace("0xC68DEAD1u", release_pa).replace("0xC68DEAD2u", stage_pa)
        if "C68DEAD" in rendered:
            print(f"C68_GATE_FAIL: placeholder remains in rendered {name}", file=sys.stderr)
            return 1
        if release_pa not in rendered or stage_pa not in rendered:
            print(f"C68_GATE_FAIL: nm PA missing from rendered {name}", file=sys.stderr)
            return 1
        (dest_dir / name).write_text(rendered, encoding="utf-8", newline="\n")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
