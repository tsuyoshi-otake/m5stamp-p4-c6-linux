#!/usr/bin/env python3
"""Check immutable carrier facts before firmware work is changed.

This is deliberately a small standard-library-only verifier. It validates the
carrier netlist and the checked-in board contract; it does not probe hardware,
flash an image, or claim Linux support.
"""

from __future__ import annotations

import json
import argparse
import hashlib
import re
import sys
from pathlib import Path


ROOT = Path(__file__).resolve().parents[1]
CONTRACT = ROOT / "board-contract.json"
SHA256 = re.compile(r"^[0-9a-f]{64}$")


def fail(message: str) -> None:
    raise SystemExit(f"ERROR: {message}")


def load(path: Path) -> dict:
    try:
        return json.loads(path.read_text(encoding="utf-8"))
    except FileNotFoundError:
        fail(f"missing required file: {path}")
    except json.JSONDecodeError as exc:
        fail(f"invalid JSON in {path}: {exc}")
    raise AssertionError("unreachable")


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser(
        description="Validate the board contract against a pinned netlist export."
    )
    parser.add_argument(
        "--netlist",
        type=Path,
        required=True,
        help="path to the Rev0.15 netlist JSON export",
    )
    return parser.parse_args()


def sha256(path: Path) -> str:
    digest = hashlib.sha256()
    with path.open("rb") as stream:
        for block in iter(lambda: stream.read(1024 * 1024), b""):
            digest.update(block)
    return digest.hexdigest()


def main() -> int:
    args = parse_args()
    netlist_path = args.netlist.expanduser().resolve()
    netlist = load(netlist_path)
    contract = load(CONTRACT)
    source_netlist = contract.get("source_netlist", {})
    expected_sha256 = source_netlist.get("sha256") if isinstance(source_netlist, dict) else None
    if not isinstance(expected_sha256, str) or not SHA256.fullmatch(expected_sha256):
        fail("board-contract.json must contain source_netlist.sha256")
    actual_sha256 = sha256(netlist_path)
    if actual_sha256 != expected_sha256:
        fail(
            f"netlist SHA-256 mismatch: {actual_sha256} != {expected_sha256} "
            f"for {netlist_path}"
        )
    required = netlist.get("requiredPaths", {})
    usb = required.get("usbA", {})
    stamp = usb.get("stampP4", {})
    u1 = {int(pin["pin"]): pin for pin in netlist.get("u1", {}).get("pins", [])}

    checks = [
        (stamp.get("vbusPin") == 15, "USB-A VBUS must terminate at Stamp-P4 pin 15"),
        (stamp.get("dPlusPin") == 17, "USB-A D+ must terminate at Stamp-P4 pin 17"),
        (stamp.get("dMinusPin") == 19, "USB-A D- must terminate at Stamp-P4 pin 19"),
        (usb.get("protectedVbus") == "USB_5V_PROTECTED", "USB-A VBUS must be protected"),
        (u1.get(17, {}).get("pcbNet") == "USB_A_D+", "U1 pin 17 net mismatch"),
        (u1.get(19, {}).get("pcbNet") == "USB_A_D-", "U1 pin 19 net mismatch"),
        (u1.get(40, {}).get("status") == "NC", "USB2 host D+ must remain NC"),
        (u1.get(41, {}).get("status") == "NC", "USB2 host D- must remain NC"),
        (contract.get("usb_a_device", {}).get("role") == "device_only_on_this_carrier", "contract must keep USB-A device-only"),
        (contract.get("power", {}).get("host_vbus_source") is False, "contract must not promise host VBUS"),
        (contract.get("power", {}).get("usb_a_and_usb_c_simultaneous_power") == "prohibited", "simultaneous USB power must remain prohibited"),
        (contract.get("linux", {}).get("status") == "not_implemented", "contract must not claim Linux is complete"),
    ]

    failures = [message for ok, message in checks if not ok]
    if failures:
        for message in failures:
            print(f"FAIL: {message}")
        return 1

    print("PASS: carrier USB, recovery, power, and Linux status contract")
    print(f"PASS: checked {netlist_path}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
