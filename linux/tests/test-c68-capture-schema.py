"""Negative/positive checks for the host-side C68 capture classifier."""

import importlib.util
from pathlib import Path


SCRIPT = Path(__file__).with_name("run-c68-0044-observe-flash-120s.py")
SPEC = importlib.util.spec_from_file_location("c68_capture", SCRIPT)
assert SPEC and SPEC.loader
MODULE = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(MODULE)


def main() -> None:
    text = """\
C68 WAIT
C68 RELEASE
C67 CALLIN cpu=1
C67 ONLINE cpu=1
smp: Brought up 1 node, 2 CPUs
Run /sbin/init
M5STAMP_TOP BEGIN label=start
CPU1 worker sample
M5STAMP_TOP END label=start
M5STAMP_STRESS topology online=0-1 processors=2 seconds=2
M5STAMP_STRESS PASS seconds=2 iterations=4 temp_reads=3 temp_failures=0 temp_start_mc=42000 temp_min_mc=41900 temp_max_mc=42100 temp_end_mc=42050
"""
    events = MODULE.extract_boot_events(text)
    names = [event["event"] for event in events]
    assert names == [
        "WAIT",
        "RELEASE",
        "CALLIN",
        "ONLINE",
        "BROUGHT_UP",
        "INIT",
        "STRESS_PASS",
    ], names
    top_blocks = MODULE.extract_top_blocks(text)
    assert top_blocks == [
        [
            "M5STAMP_TOP BEGIN label=start",
            "CPU1 worker sample",
            "M5STAMP_TOP END label=start",
        ]
    ], top_blocks
    assert MODULE.extract_top_blocks("M5STAMP_TOP BEGIN label=lost\n") == []
    assert MODULE.extract_stress_summaries(text) == [
        {
            "line": 11,
            "fields": {
                "seconds": 2,
                "iterations": 4,
                "temp_reads": 3,
                "temp_failures": 0,
                "temp_start_mc": 42000,
                "temp_min_mc": 41900,
                "temp_max_mc": 42100,
                "temp_end_mc": 42050,
            },
        }
    ]
    tsens_text = (
        "P4_TSENS_REF PASS efuse_raw=0x101 efuse_sign=1 delta_t=-1 "
        "range_offset=0 range_reg=15 dac_before=7 dac_probe=7 "
        "dac_readback=15 ready=1 raw_min=100 raw_max=101 "
        "raw_avg=100 temp_mc=23440\n"
        "P4_TSENS_REF CLEANUP sensor=off clock=off regi2c=off\n"
    )
    assert MODULE.extract_tsens_reference(tsens_text) == [
        {
            "line": 1,
            "status": "PASS",
            "fields": {
                "efuse_raw": 0x101,
                "efuse_sign": 1,
                "delta_t": -1,
                "range_offset": 0,
                "range_reg": 15,
                "dac_before": 7,
                "dac_probe": 7,
                "dac_readback": 15,
                "ready": 1,
                "raw_min": 100,
                "raw_max": 101,
                "raw_avg": 100,
                "temp_mc": 23440,
            },
        }
    ]
    linux_tsens_text = (
        "P4_TSENS_LINUX PASS efuse_raw=0x101 delta_t=-1 range_reg=15 "
        "dac_before=15 dac_probe=7 dac_readback=15 ready=0 raw=101 "
        "temp_mc=23000\n"
    )
    assert MODULE.extract_tsens_linux(linux_tsens_text) == [
        {
            "line": 1,
            "fields": {
                "efuse_raw": 0x101,
                "delta_t": -1,
                "range_reg": 15,
                "dac_before": 15,
                "dac_probe": 7,
                "dac_readback": 15,
                "ready": 0,
                "raw": 101,
                "temp_mc": 23000,
            },
        }
    ]
    assert MODULE.validate_tsens_capture(
        MODULE.extract_tsens_reference(tsens_text),
        MODULE.extract_tsens_linux(linux_tsens_text),
        MODULE.extract_stress_summaries(text),
        True,
    ) == []
    bad_linux = MODULE.extract_tsens_linux(linux_tsens_text)
    bad_linux[0]["fields"]["delta_t"] = 34
    assert "delta_t-mismatch" in MODULE.validate_tsens_capture(
        MODULE.extract_tsens_reference(tsens_text),
        bad_linux,
        MODULE.extract_stress_summaries(text),
        True,
    )
    bad_stress = MODULE.extract_stress_summaries(text)
    bad_stress[0]["fields"]["temp_failures"] = 1
    assert "stress-temp-read-failures" in MODULE.validate_tsens_capture(
        MODULE.extract_tsens_reference(tsens_text),
        MODULE.extract_tsens_linux(linux_tsens_text),
        bad_stress,
        True,
    )
    assert MODULE.classify_capture(text, "/usr/sbin/m5stamp-smp-stress", True) == (
        "helper-pass"
    )
    assert MODULE.classify_capture(
        text, "/usr/sbin/m5stamp-smp-stress", False
    ) == "no-command-sent"
    assert MODULE.classify_capture(
        "M5STAMP_STRESS FAIL crosscall iteration=1\n",
        "/usr/sbin/m5stamp-smp-stress",
        True,
    ) == "stress-fail"
    assert MODULE.classify_capture(
        "M5STAMP_STRESS topology online=0-1 processors=2\n"
        "M5STAMP_STRESS WORKER_START cpu=1 seconds=120\n",
        "/usr/sbin/m5stamp-smp-stress",
        True,
    ) == "stress-incomplete"
    assert MODULE.classify_capture(
        "Kernel panic - not syncing\n",
        "/usr/sbin/m5stamp-smp-stress",
        False,
    ) == "panic"
    assert MODULE.classify_capture(
        "rcu: INFO: rcu_sched detected stalls on CPUs/tasks:\n",
        "/usr/sbin/m5stamp-smp-stress",
        False,
    ) == "rcu-stall"
    assert MODULE.classify_capture("HP_SYS_HP_WDT_RESET\n", None, False) == (
        "watchdog-reset"
    )
    print("C68 capture schema checks: PASS")


if __name__ == "__main__":
    main()
