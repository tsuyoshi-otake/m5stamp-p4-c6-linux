#!/usr/bin/env python3
"""Check a captured serial boot log for the smoke-test markers (#28).

Capture the console with capture-boot.ps1 (it opens the port without
resetting the board), then run:

    python3 linux/tests/check-boot-log.py boot.bin
    python3 linux/tests/check-boot-log.py --after-reboot boot-after-reboot.bin

--after-reboot is for the capture that follows a plain `reboot`; that boot
must not report a crash capsule (#19).  Prints one PASS/FAIL line per check
and exits non-zero when any check fails.
"""

from __future__ import annotations

import argparse
import re
import sys
from pathlib import Path

# Lines that must appear: SSH (inetd) came up and the C6 produced wlan0.
REQUIRED = {
    "ssh started": re.compile(r"ES_HOSTKEY INETD_(START|ALREADY)"),
    "wlan0 appeared": re.compile(r"M3-lab: wlan0 appeared"),
    "network up": re.compile(
        r"M3-lab: (SoftAP started|association complete)"),
}

# Lines that must not appear in any boot.
FORBIDDEN = {
    "init marker failure": re.compile(r"\bES_[A-Z_]+ [A-Z_]*FAIL\b"),
    "missing applet": re.compile(r"applet not found"),
    "missing command": re.compile(r"can't execute|: not found$"),
    "kernel oops": re.compile(r"Kernel panic|\bOops\b|\bBUG:"),
    "C6 firmware mismatch": re.compile(
        r"Incompatible ESP Host-firmware release detected"),
    "wlan0 missing": re.compile(r"M3-lab: wlan0 did not (appear|associate)"),
    "DHCP failure": re.compile(r"M3-lab: DHCP failed"),
    "wpa_supplicant failure": re.compile(r"M3-lab: wpa_supplicant (fallback )?failed"),
    # #5: CPU1 must read back CPU0's PMA/PMP (SMP boot-shim only).
    "CPU1 protection mismatch": re.compile(r"C68 CPU1 PMA/PMP diff=(?!0{8}\b)"),
}

# Reported but not failing: they depend on the config.txt under test.
WARN = {
    "config error": re.compile(r"M3-lab: CONFIG ERROR"),
}

CRASH_CAPSULE = re.compile(r"CRASH_CAPSULE VALID")
KERNEL_TIME = re.compile(r"^\[\s*(\d+\.\d+)\]")
# #5: a boot-shim that parks CPU1 must also report CPU1's PMA/PMP copy.
CPU1_WAITING = re.compile(r"C68 CPU1 WAITING")
CPU1_PROT = re.compile(r"C68 CPU1 PMA/PMP diff=")


def read_lines(path: Path) -> list[str]:
    text = path.read_bytes().decode("utf-8", errors="replace")
    return [line.rstrip("\r") for line in text.split("\n")]


def check(lines: list[str], after_reboot: bool) -> list[tuple[bool, str]]:
    results: list[tuple[bool, str]] = []

    for name, pattern in REQUIRED.items():
        hit = next((line for line in lines if pattern.search(line)), None)
        results.append((hit is not None, f"{name}" + (f": {hit.strip()}" if hit else "")))

    for name, pattern in FORBIDDEN.items():
        hits = [line.strip() for line in lines if pattern.search(line)]
        results.append((not hits, f"no {name}" + (f": {hits[0]}" if hits else "")))

    if any(CPU1_WAITING.search(line) for line in lines):
        hit = next((line for line in lines if CPU1_PROT.search(line)), None)
        results.append((hit is not None,
                        "CPU1 protection reported" + (f": {hit.strip()}" if hit else "")))

    if after_reboot:
        hits = [line.strip() for line in lines if CRASH_CAPSULE.search(line)]
        results.append((not hits,
                        "no crash capsule after reboot"
                        + (f": {hits[0][:120]}" if hits else "")))
    return results


def ssh_ready_seconds(lines: list[str]) -> float | None:
    """Last kernel timestamp before inetd started; init output has none."""
    last = None
    for line in lines:
        match = KERNEL_TIME.match(line)
        if match:
            last = float(match.group(1))
        if REQUIRED["ssh started"].search(line):
            return last
    return None


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("capture", type=Path)
    parser.add_argument("--after-reboot", action="store_true",
                        help="the capture follows a plain reboot; fail on a crash capsule")
    args = parser.parse_args(argv)

    lines = read_lines(args.capture)
    results = check(lines, args.after_reboot)
    for ok, message in results:
        print(f"{'PASS' if ok else 'FAIL'}: {message}")
    for name, pattern in WARN.items():
        for line in lines:
            if pattern.search(line):
                print(f"WARN: {name}: {line.strip()}")
    ready = ssh_ready_seconds(lines)
    if ready is not None:
        print(f"INFO: SSH started after kernel time {ready:.1f} s or later")
    return 0 if all(ok for ok, _ in results) else 1


if __name__ == "__main__":
    sys.exit(main())
