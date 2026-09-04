#!/usr/bin/env python3
"""Fail-closed layout checks for easystick_cmd53_bb in boot-shim ELF/map.

Expects nm -S output (addr size type name) so the real symbol size is used
instead of a hand-picked BB_SIZE ceiling.
"""
from __future__ import annotations

from pathlib import Path
import re
import sys

LP_ROM_BSS_FLOOR = 0x5010FA80
LP_RAM_LO = 0x50108000
LP_RAM_HI = 0x50110000
# Shared header + kernel duplicate must use the complete v6 request-end layout.
# An upper bound would allow an older, truncated duplicate to pass silently.
BB_SIZE_EXACT = 0x120
CMD52_MARKER_WORDS_EXACT = 6
CMD52_MARKER_SIZE_EXACT = CMD52_MARKER_WORDS_EXACT * 4
BEAT_CPUS_EXACT = 2
BEAT_SIZE_EXACT = BEAT_CPUS_EXACT * 16
WINDOW_SIZE_EXACT = BB_SIZE_EXACT + CMD52_MARKER_SIZE_EXACT + BEAT_SIZE_EXACT
WINDOW_BEAT_OFF = BB_SIZE_EXACT + CMD52_MARKER_SIZE_EXACT


def parse_nm_symbol(nm_text: str, name: str) -> tuple[int, int]:
    for line in nm_text.splitlines():
        parts = line.split()
        # nm -S: ADDR SIZE TYPE NAME
        if len(parts) >= 4 and parts[-1] == name:
            return int(parts[0], 16), int(parts[1], 16)
        # plain nm fallback: ADDR TYPE NAME (reject — size required)
        if len(parts) == 3 and parts[-1] == name:
            raise SystemExit(
                "CMD53_BB_GATE_FAIL: nm output lacks size; use nm -S"
            )
    raise SystemExit(f"CMD53_BB_GATE_FAIL: nm missing {name}")


def parse_map_rtc_noinit(map_text: str) -> tuple[int, int]:
    """Return [start, end) of .rtc_noinit if present."""
    m = re.search(
        r"\.rtc_noinit\s+0x([0-9a-fA-F]+)\s+0x([0-9a-fA-F]+)",
        map_text,
    )
    if not m:
        m = re.search(r"\.rtc_noinit\s+0x([0-9a-fA-F]+)", map_text)
        if not m:
            raise SystemExit("CMD53_BB_GATE_FAIL: .rtc_noinit missing from map")
        start = int(m.group(1), 16)
        m2 = re.search(
            r"\.rtc_noinit\s+0x[0-9a-fA-F]+\s+0x([0-9a-fA-F]+)",
            map_text,
        )
        if not m2:
            raise SystemExit("CMD53_BB_GATE_FAIL: .rtc_noinit size missing from map")
        size = int(m2.group(1), 16)
        return start, start + size
    start = int(m.group(1), 16)
    size = int(m.group(2), 16)
    return start, start + size


def main() -> int:
    argv = list(sys.argv[1:])
    require_cmd52_marker = "--require-cmd52-marker" in argv
    if require_cmd52_marker:
        argv.remove("--require-cmd52-marker")
    if not argv or len(argv) > 2:
        raise SystemExit(
            "CMD53_BB_GATE_FAIL: usage nm [-map] [--require-cmd52-marker]"
        )

    nm_path = Path(argv[0])
    map_path = Path(argv[1]) if len(argv) > 1 else None
    nm_text = nm_path.read_text(encoding="utf-8", errors="replace")
    window = None
    try:
        window, window_size = parse_nm_symbol(nm_text, "easystick_rtc_c")
    except SystemExit:
        window = None
        window_size = 0
    if window is not None:
        print(
            f"RTC_C_WINDOW_NM pa=0x{window:08x} "
            f"size=0x{window_size:x} ({window_size})"
        )
        if window_size != WINDOW_SIZE_EXACT:
            print(
                f"CMD53_BB_GATE_FAIL: unexpected window size {window_size}; "
                f"expected {WINDOW_SIZE_EXACT}",
                file=sys.stderr,
            )
            return 1
        bb = window
        bb_size = BB_SIZE_EXACT
        marker = window + BB_SIZE_EXACT
        marker_size = CMD52_MARKER_SIZE_EXACT
        beat = window + WINDOW_BEAT_OFF
        beat_size = BEAT_SIZE_EXACT
        print(f"CMD53_BB_NM pa=0x{bb:08x} size=0x{bb_size:x} ({bb_size})")
        if require_cmd52_marker:
            print(
                f"CMD52_MARKER_NM pa=0x{marker:08x} "
                f"size=0x{marker_size:x} ({marker_size})"
            )
        print(f"BEAT_NM pa=0x{beat:08x} size=0x{beat_size:x} ({beat_size})")
    else:
        bb, bb_size = parse_nm_symbol(nm_text, "easystick_cmd53_bb")
        print(f"CMD53_BB_NM pa=0x{bb:08x} size=0x{bb_size:x} ({bb_size})")

        if bb_size != BB_SIZE_EXACT:
            print(
                f"CMD53_BB_GATE_FAIL: unexpected symbol size {bb_size}",
                file=sys.stderr,
            )
            return 1

        marker = marker_size = None
        if require_cmd52_marker:
            marker, marker_size = parse_nm_symbol(nm_text, "easystick_cmd52_marker")
            print(
                f"CMD52_MARKER_NM pa=0x{marker:08x} "
                f"size=0x{marker_size:x} ({marker_size})"
            )
            if marker_size != CMD52_MARKER_SIZE_EXACT:
                print(
                    "CMD52_MARKER_GATE_FAIL: unexpected symbol size "
                    f"{marker_size}; expected {CMD52_MARKER_WORDS_EXACT} words",
                    file=sys.stderr,
                )
                return 1
            if marker != bb + BB_SIZE_EXACT:
                print(
                    "CMD52_MARKER_GATE_FAIL: marker is not adjacent to exact BB end "
                    f"(bb_end=0x{bb + BB_SIZE_EXACT:08x}, marker=0x{marker:08x})",
                    file=sys.stderr,
                )
                return 1
            if marker < LP_RAM_LO or marker + marker_size > LP_ROM_BSS_FLOOR:
                print(
                    "CMD52_MARKER_GATE_FAIL: marker outside LP SRAM/ROM-BSS bounds",
                    file=sys.stderr,
                )
                return 1

        beat, beat_size = parse_nm_symbol(nm_text, "easystick_beat")
        print(f"BEAT_NM pa=0x{beat:08x} size=0x{beat_size:x} ({beat_size})")
        if beat_size != BEAT_SIZE_EXACT:
            print(
                f"BEAT_GATE_FAIL: unexpected symbol size {beat_size}; "
                f"expected {BEAT_SIZE_EXACT}",
                file=sys.stderr,
            )
            return 1
        beat_expect = bb + BB_SIZE_EXACT + CMD52_MARKER_SIZE_EXACT
        if require_cmd52_marker:
            beat_expect = marker + marker_size
        if beat != beat_expect:
            print(
                "BEAT_GATE_FAIL: beat is not after BB+marker hole "
                f"(expect=0x{beat_expect:08x}, beat=0x{beat:08x})",
                file=sys.stderr,
            )
            return 1

    if bb < LP_RAM_LO or bb >= LP_RAM_HI:
        print("CMD53_BB_GATE_FAIL: symbol outside LP SRAM window", file=sys.stderr)
        return 1
    if bb + bb_size > LP_ROM_BSS_FLOOR:
        print("CMD53_BB_GATE_FAIL: symbol+size crosses LP ROM BSS floor", file=sys.stderr)
        return 1
    if beat < LP_RAM_LO or beat + beat_size > LP_ROM_BSS_FLOOR:
        print("BEAT_GATE_FAIL: beat outside LP SRAM/ROM-BSS bounds", file=sys.stderr)
        return 1

    if map_path and map_path.is_file():
        start, end = parse_map_rtc_noinit(
            map_path.read_text(encoding="utf-8", errors="replace")
        )
        print(f"CMD53_BB_MAP rtc_noinit=[0x{start:08x},0x{end:08x})")
        if bb < start or bb + bb_size > end:
            print("CMD53_BB_GATE_FAIL: symbol outside .rtc_noinit", file=sys.stderr)
            return 1
        if window is not None and (
            window < start or window + window_size > end
        ):
            print("CMD53_BB_GATE_FAIL: window outside .rtc_noinit", file=sys.stderr)
            return 1
        if require_cmd52_marker and (
            marker < start or marker + marker_size > end
        ):
            print(
                "CMD52_MARKER_GATE_FAIL: symbol outside .rtc_noinit",
                file=sys.stderr,
            )
            return 1
        if beat < start or beat + beat_size > end:
            print("BEAT_GATE_FAIL: symbol outside .rtc_noinit", file=sys.stderr)
            return 1
        if end > LP_ROM_BSS_FLOOR:
            print("CMD53_BB_GATE_FAIL: .rtc_noinit end past LP ROM BSS floor", file=sys.stderr)
            return 1
    else:
        print("CMD53_BB_MAP skipped (no map file); nm LP-window checks only")

    print(f"CMD53_BB_SIZE={bb_size}")
    print(f"0x{bb:08x}u")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
