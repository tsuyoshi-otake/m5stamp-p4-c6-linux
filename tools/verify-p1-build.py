#!/usr/bin/env python3
"""Fail-closed checks for the P1 source, device tree, and image inputs.

This verifier deliberately checks only the invariants named here.  It does
not prove electrical behaviour, supplier acceptance, or a successful UART
run; those require the separate P1 hardware evidence record.
"""

from __future__ import annotations

import argparse
import hashlib
import re
import subprocess
import sys
from pathlib import Path


EXPECTED_ZEPHYR = "d544481d9ad9c711cefe984c5ea926d71cb56341"
EXPECTED_C6 = "3f0d1076749afdb589f00c075d8dce895e3dd32d"
EXPECTED_IDF = "2c211b236707889e8400c4dc5644dd5c4ee071e0"
EXPECTED_PROTOBUF = "abc67a11c6db271bedbb9f58be85d6f4e2ea8389"


def git_head(path: Path) -> str:
    return subprocess.check_output(
        ("git", "-C", str(path), "rev-parse", "HEAD"),
        text=True,
        stderr=subprocess.STDOUT,
    ).strip()


def sha256(path: Path) -> str:
    digest = hashlib.sha256()
    with path.open("rb") as stream:
        for chunk in iter(lambda: stream.read(1024 * 1024), b""):
            digest.update(chunk)
    return digest.hexdigest()


def require(text: str, pattern: str, label: str, failures: list[str]) -> None:
    if re.search(pattern, text, re.MULTILINE) is None:
        failures.append(f"missing {label}: {pattern}")


def dts_number(value: int) -> str:
    """Match the decimal or hexadecimal spelling emitted by dtc."""
    return rf"(?:{value}|0x{value:x})"


def node_body(text: str, marker: str) -> str:
    """Return one DTS node body using balanced braces."""
    start = text.find(marker)
    if start < 0:
        raise ValueError(f"node marker not found: {marker}")
    opening = text.find("{", start)
    if opening < 0:
        raise ValueError(f"node opening brace not found: {marker}")
    depth = 0
    for index in range(opening, len(text)):
        if text[index] == "{":
            depth += 1
        elif text[index] == "}":
            depth -= 1
            if depth == 0:
                return text[opening + 1 : index]
    raise ValueError(f"unterminated node: {marker}")


def check_overlay(path: Path, failures: list[str]) -> None:
    text = path.read_text(encoding="utf-8")
    exact = {
        r"&sdhc0\s*\{[^}]*status\s*=\s*\"disabled\"\s*;": "sdhc0 disabled",
        r"&sdhc1\s*\{[^}]*status\s*=\s*\"okay\"\s*;": "sdhc1 enabled",
        r"&sdhc1\s*\{[^}]*bus-width\s*=\s*<4>\s*;": "SDIO 4-bit",
        r"&sdhc1\s*\{[^}]*clk-pin\s*=\s*<43>\s*;": "SDIO CLK GPIO43",
        r"&sdhc1\s*\{[^}]*cmd-pin\s*=\s*<44>\s*;": "SDIO CMD GPIO44",
        r"&sdhc1\s*\{[^}]*d0-pin\s*=\s*<45>\s*;": "SDIO D0 GPIO45",
        r"&sdhc1\s*\{[^}]*d1-pin\s*=\s*<46>\s*;": "SDIO D1 GPIO46",
        r"&sdhc1\s*\{[^}]*d2-pin\s*=\s*<47>\s*;": "SDIO D2 GPIO47",
        r"&sdhc1\s*\{[^}]*d3-pin\s*=\s*<48>\s*;": "SDIO D3 GPIO48",
        (
            r"&esp_hosted_mcu\s*\{[^}]*reset-gpios\s*=\s*"
            r"<&gpio1\s+10\s+GPIO_ACTIVE_HIGH>\s*;"
        ): "C6 GPIO42 active-high reset/power gate",
        r"&esp_hosted_mcu_hci\s*\{[^}]*status\s*=\s*\"disabled\"\s*;": (
            "Bluetooth disabled"
        ),
        r"&mdio\s*\{[^}]*status\s*=\s*\"disabled\"\s*;": "MDIO disabled",
        r"&phy\s*\{[^}]*status\s*=\s*\"disabled\"\s*;": "Ethernet PHY disabled",
        r"&eth\s*\{[^}]*status\s*=\s*\"disabled\"\s*;": "Ethernet disabled",
    }
    for pattern, label in exact.items():
        require(text, pattern, label, failures)
    if re.search(r"\bpwr-gpios\s*=", text):
        failures.append("overlay must not define pwr-gpios")


def check_dts(path: Path, failures: list[str]) -> None:
    text = path.read_text(encoding="utf-8")
    try:
        sdhc0 = node_body(text, "sdhc0:")
        sdhc1 = node_body(text, "sdhc1:")
        hosted = node_body(text, "esp_hosted_mcu:")
        hci = node_body(text, "esp_hosted_mcu_hci:")
        mdio = node_body(text, "mdio:")
        phy = node_body(text, "phy:")
        eth = node_body(text, "eth:")
    except (OSError, ValueError) as exc:
        failures.append(f"generated DTS cannot be inspected: {exc}")
        return

    require(sdhc0, r'\bstatus\s*=\s*"disabled"\s*;', "generated sdhc0 disabled", failures)
    require(sdhc1, r'\bstatus\s*=\s*"okay"\s*;', "generated sdhc1 enabled", failures)
    require(
        sdhc1,
        rf"\bbus-width\s*=\s*<\s*{dts_number(4)}\s*>\s*;",
        "generated SDIO 4-bit",
        failures,
    )
    for name, value in (
        ("clk-pin", 43),
        ("cmd-pin", 44),
        ("d0-pin", 45),
        ("d1-pin", 46),
        ("d2-pin", 47),
        ("d3-pin", 48),
    ):
        require(
            sdhc1,
            rf"\b{name}\s*=\s*<\s*{dts_number(value)}\s*>\s*;",
            f"generated {name} GPIO{value}",
            failures,
        )
    require(
        hosted,
        rf"\breset-gpios\s*=\s*<\s*&gpio1\s+{dts_number(10)}\s+"
        rf"(?:GPIO_ACTIVE_HIGH|{dts_number(0)})\s*>\s*;",
        "generated C6 active-high GPIO42",
        failures,
    )
    require(hci, r'\bstatus\s*=\s*"disabled"\s*;', "generated Bluetooth disabled", failures)
    require(mdio, r'\bstatus\s*=\s*"disabled"\s*;', "generated MDIO disabled", failures)
    require(phy, r'\bstatus\s*=\s*"disabled"\s*;', "generated Ethernet PHY disabled", failures)
    require(eth, r'\bstatus\s*=\s*"disabled"\s*;', "generated Ethernet disabled", failures)


def check_config(path: Path, failures: list[str]) -> None:
    lines = {
        line.split("=", 1)[0]: line.split("=", 1)[1]
        for line in path.read_text(encoding="utf-8").splitlines()
        if "=" in line and not line.lstrip().startswith("#")
    }
    expected = {
        "CONFIG_ESP_HOSTED_MCU_P1_TRANSPORT_TEST": "y",
        "CONFIG_ESP_HOSTED_MCU_FW_VERSION_MAJOR": "2",
        "CONFIG_ESP_HOSTED_MCU_FW_VERSION_MINOR": "12",
        "CONFIG_ESP_HOSTED_MCU_FW_VERSION_PATCH": "12",
        "CONFIG_WIFI_ESP_HOSTED_MCU": "y",
        "CONFIG_WIFI_USAGE_MODE_STA": "y",
        "CONFIG_NET_DHCPV4": "y",
    }
    for key, value in expected.items():
        if lines.get(key) != value:
            failures.append(f"{path.name}: {key}={value} is required")


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--zephyr-source", type=Path, required=True)
    parser.add_argument("--c6-source", type=Path, required=True)
    parser.add_argument("--idf-source", type=Path, required=True)
    parser.add_argument("--protobuf-source", type=Path, required=True)
    parser.add_argument("--overlay", type=Path, required=True)
    parser.add_argument("--zephyr-dts", type=Path, required=True)
    parser.add_argument("--zephyr-config", type=Path, required=True)
    parser.add_argument("--zephyr-bin", type=Path, required=True)
    parser.add_argument("--c6-bin", type=Path, required=True)
    args = parser.parse_args()

    failures: list[str] = []
    revisions = (
        (args.zephyr_source, EXPECTED_ZEPHYR, "Zephyr"),
        (args.c6_source, EXPECTED_C6, "ESP-Hosted-MCU"),
        (args.idf_source, EXPECTED_IDF, "ESP-IDF"),
        (args.protobuf_source, EXPECTED_PROTOBUF, "protobuf-c"),
    )
    for source, expected, label in revisions:
        try:
            actual = git_head(source)
        except (OSError, subprocess.CalledProcessError) as exc:
            failures.append(f"{label} revision cannot be read: {exc}")
            continue
        if actual != expected:
            failures.append(f"{label} revision {actual} != {expected}")

    for path, label in (
        (args.overlay, "overlay"),
        (args.zephyr_dts, "generated DTS"),
        (args.zephyr_config, "generated config"),
        (args.zephyr_bin, "Zephyr image"),
        (args.c6_bin, "C6 image"),
    ):
        if not path.is_file():
            failures.append(f"{label} is missing: {path}")

    if args.overlay.is_file():
        check_overlay(args.overlay, failures)
    if args.zephyr_dts.is_file():
        check_dts(args.zephyr_dts, failures)
    if args.zephyr_config.is_file():
        check_config(args.zephyr_config, failures)

    if failures:
        print("\n".join(f"FAIL: {failure}" for failure in failures))
        return 1

    print(f"PASS: P1 source, DTS, config, and images verified")
    print(f"ZEPHYR_BIN_SHA256={sha256(args.zephyr_bin)}")
    print(f"C6_BIN_SHA256={sha256(args.c6_bin)}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
