#!/usr/bin/env python3
"""Independent expected-value vectors for the P4 TSENS integer conversion."""

from __future__ import annotations


def decode_delta_t(efuse_raw: int) -> int:
    """ESP-IDF's bit-8 sign plus low-8 magnitude convention."""
    magnitude = efuse_raw & 0xFF
    return -magnitude if efuse_raw & (1 << 8) else magnitude


def div_round_closest(numerator: int, denominator: int) -> int:
    if numerator >= 0:
        return (numerator + denominator // 2) // denominator
    return -((-numerator + denominator // 2) // denominator)


def raw_to_mdeg(raw: int, delta_t: int) -> int:
    """Independent integer oracle for fixed offset=0 / DAC=15."""
    return (
        div_round_closest(raw * 4386, 10)
        - 20520
        - delta_t * 100
    )


def main() -> None:
    decode_vectors = {
        0x001: 1,
        0x07F: 127,
        0x101: -1,
        0x17F: -127,
        0x200: 0,
        0x3FF: -255,
    }
    for efuse_raw, expected in decode_vectors.items():
        actual = decode_delta_t(efuse_raw)
        assert actual == expected, (
            f"eFuse 0x{efuse_raw:03x}: {actual} != {expected}"
        )

    conversion_vectors = {
        (100, 0): 23340,
        (101, 33): 20479,
        (101, -33): 27079,
        (0, 0): -20520,
    }
    for (raw, delta_t), expected in conversion_vectors.items():
        actual = raw_to_mdeg(raw, delta_t)
        assert actual == expected, (
            f"raw={raw} delta_t={delta_t}: {actual} != {expected}"
        )

    print("PASS: TSENS independent eFuse/conversion vectors")


if __name__ == "__main__":
    main()
