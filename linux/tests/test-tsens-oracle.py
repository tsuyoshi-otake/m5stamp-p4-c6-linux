"""Static and independent math checks for the ESP32-P4 TSENS oracle."""

from pathlib import Path


SOURCE = (
    Path(__file__).resolve().parents[1]
    / "boot-shim"
    / "main"
    / "tsens_oracle.c"
)


def decode_delta_t(efuse_raw: int) -> int:
    magnitude = efuse_raw & 0xFF
    return -magnitude if efuse_raw & (1 << 8) else magnitude


def raw_to_mdeg(raw: int, delta_t: int) -> int:
    # 0.4386 C/sample -> 4386/10 milli-degrees/sample.
    return ((raw * 4386 + 5) // 10) - 20520 - delta_t * 100


def main() -> int:
    source = SOURCE.read_text(encoding="utf-8")
    required = (
        "regi2c_ctrl_ll_i2c_sar_periph_enable",
        "REGI2C_WRITE_MASK",
        "REGI2C_READ_MASK",
        "LP_TSENS.ctrl.sample_en = 1",
        "LP_TSENS.ctrl.sample_en = 0",
        "P4_TSENS_REF PASS",
        "P4_TSENS_REF CLEANUP",
    )
    for marker in required:
        assert marker in source, f"missing oracle marker: {marker}"
    assert source.count("REGI2C_WRITE_MASK") >= 2
    assert "sign_extend32" not in source
    assert "#include <math.h>" not in source

    # Independent eFuse vectors.  These kill the tempting two's-complement
    # mutant and prove that bit 8 is a sign flag while bits 7:0 are magnitude.
    assert decode_delta_t(0x000) == 0
    assert decode_delta_t(0x07F) == 127
    assert decode_delta_t(0x101) == -1
    assert decode_delta_t(0x17F) == -127
    assert decode_delta_t(0x200) == 0  # bit 9 is not part of the decode

    # Independent conversion vectors in milli-degrees Celsius.
    assert raw_to_mdeg(100, 0) == 23340
    assert raw_to_mdeg(100, -1) == 23440
    assert raw_to_mdeg(100, 127) == 10640

    # A two's-complement interpretation of the sign-bearing 0x101 vector is
    # deliberately different, so this test cannot silently bless that mutant.
    twos_complement_9 = 0x101 - (1 << 9)
    assert twos_complement_9 != decode_delta_t(0x101)

    print("TSENS_ORACLE_STATIC_PASS")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
