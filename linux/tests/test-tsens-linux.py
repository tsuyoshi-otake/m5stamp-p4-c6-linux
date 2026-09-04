#!/usr/bin/env python3
"""Static contract and negative tests for the experimental P4 TSENS driver."""

from __future__ import annotations

from pathlib import Path
import re


ROOT = Path(__file__).resolve().parents[1]
PATCH = ROOT / "kernel-patches" / "0050-thermal-esp32p4-add-lp-tsens-driver.patch"


def driver_source() -> str:
    text = PATCH.read_text(encoding="utf-8")
    marker = "+++ b/drivers/thermal/esp32p4_tsens.c\n"
    payload = text.split(marker, 1)[1].split("\n", 1)[1]
    lines = []
    for line in payload.splitlines():
        if line.startswith("+"):
            lines.append(line[1:])
        elif line == r"\ No newline at end of file":
            continue
        else:
            break
    return "\n".join(lines) + "\n"


def declared_driver_line_count(text: str | None = None) -> int:
    if text is None:
        text = PATCH.read_text(encoding="utf-8")
    match = re.search(r"@@ -0,0 \+1,(\d+) @@\n", text)
    assert match, "driver hunk header is missing"
    return int(match.group(1))


def validate_contract(source: str) -> list[str]:
    required = {
        "ESP32-P4 TSENS driver": "compatible = \"espressif,esp32p4-tsens\"",
        "analog master command register": "#define ESP32P4_ANA_I2C0_CTRL\t\t\t0x00",
        "analog master ANA_CONF1 register": "#define ESP32P4_ANA_CONF1\t\t\t0x1c",
        "analog master ANA_CONF2 register": "#define ESP32P4_ANA_CONF2\t\t\t0x20",
        "analog master clock register": "#define ESP32P4_ANA_CLK160M\t\t\t0x34",
        "analog master clock select": "ESP32P4_ANA_CLK_I2C_MST_SEL_160M",
        "analog master reset": "#define ESP32P4_LPPERI_I2CMST_RESET\t\tBIT(26)",
        "REGI2C busy bit": "#define ESP32P4_ANA_I2C_BUSY\t\t\tBIT(25)",
        "REGI2C write bit": "#define ESP32P4_ANA_I2C_WRITE\t\t\tBIT(24)",
        "REGI2C data field": "#define ESP32P4_ANA_I2C_DATA\t\t\tGENMASK(23, 16)",
        "REGI2C address field": "#define ESP32P4_ANA_I2C_ADDR\t\t\tGENMASK(15, 8)",
        "REGI2C slave field": "#define ESP32P4_ANA_I2C_SLAVE\t\t\tGENMASK(7, 0)",
        "SAR master selection": "#define ESP32P4_ANA_SAR_SELECT\t\t\tBIT(7)",
        "DAC probe write": "ESP32P4_TSENS_DAC_PROBE",
        "DAC probe readback": "dac_probe != ESP32P4_TSENS_DAC_PROBE",
        "DAC final readback": "dac_after != ESP32P4_TSENS_DAC_VALUE",
        "eFuse sign flag": "data->delta_t = (efuse_raw & BIT(8)) ?",
        "eFuse magnitude": "(efuse_raw & GENMASK(7, 0))",
        "eFuse field width": "#define ESP32P4_EFUSE_TEMP_MASK\t\t\tGENMASK(9, 0)",
        "fixed settle": "udelay(ESP32P4_TSENS_SETTLE_US)",
        "integer conversion": "ESP32P4_TSENS_ADC_MILLI_NUM",
        "thermal sysfs zone": "thermal_tripless_zone_device_register",
    }
    errors = [name for name, needle in required.items() if needle not in source]
    if "sign_extend32" in source:
        errors.append("forbidden two's-complement eFuse decoder")
    if re.search(r"\b(?:float|double)\b", source):
        errors.append("floating-point kernel conversion")
    return errors


def main() -> None:
    source = driver_source()
    errors = validate_contract(source)
    assert not errors, errors
    assert declared_driver_line_count() == len(source.splitlines()), (
        "driver hunk line count does not match extracted source"
    )

    # Deliberate mutants: each must be rejected by the same contract gate.
    bad_sign = source.replace(
        "data->delta_t = (efuse_raw & BIT(8)) ?",
        "data->delta_t = (efuse_raw & BIT(7)) ?",
        1,
    )
    assert validate_contract(bad_sign), "eFuse sign mutant was not rejected"

    bad_dac = source.replace(
        "dac_after != ESP32P4_TSENS_DAC_VALUE",
        "false",
        1,
    )
    assert validate_contract(bad_dac), "DAC readback mutant was not rejected"

    bad_settle = source.replace(
        "udelay(ESP32P4_TSENS_SETTLE_US);",
        "udelay(1);",
        1,
    )
    assert validate_contract(bad_settle), "settle-time mutant was not rejected"

    bad_offset = source.replace(
        "#define ESP32P4_ANA_I2C0_CTRL\t\t\t0x00",
        "#define ESP32P4_ANA_I2C0_CTRL\t\t\t0x14",
        1,
    )
    assert validate_contract(bad_offset), "REGI2C offset mutant was not rejected"

    bad_efuse_width = source.replace(
        "#define ESP32P4_EFUSE_TEMP_MASK\t\t\tGENMASK(9, 0)",
        "#define ESP32P4_EFUSE_TEMP_MASK\t\t\tGENMASK(8, 0)",
        1,
    )
    assert validate_contract(bad_efuse_width), (
        "eFuse field-width mutant was not rejected"
    )

    patch_text = PATCH.read_text(encoding="utf-8")
    original_count = declared_driver_line_count(patch_text)
    bad_header = patch_text.replace(
        f"+1,{original_count}", f"+1,{original_count - 1}", 1
    )
    assert declared_driver_line_count(bad_header) != len(source.splitlines()), (
        "driver hunk-count mutant was not rejected"
    )

    print("PASS: TSENS Linux driver contract and 6 negative mutants")


if __name__ == "__main__":
    main()
