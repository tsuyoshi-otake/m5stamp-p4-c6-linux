#!/usr/bin/env python3
"""Self-test for check-boot-log.py with synthetic captures."""

import importlib.util
import io
import tempfile
from contextlib import redirect_stdout
from pathlib import Path

HERE = Path(__file__).resolve().parent
spec = importlib.util.spec_from_file_location("check_boot_log", HERE / "check-boot-log.py")
cbl = importlib.util.module_from_spec(spec)
spec.loader.exec_module(cbl)

GOOD = b"""easystick-boot: CRASH_CAPSULE empty/invalid\r
[    0.000000] Linux version 6.18.35 (builder@x) #1 SMP\r
[    9.120000] esp32_sdio: loading out-of-tree module taints kernel.\r
M3-lab: wlan0 appeared\r
M3-lab: starting SoftAP mode from /tmp/wpa_supplicant.conf\r
M3-lab: SoftAP started (SSID: m5-ab12, IP: 192.168.4.1)\r
[   14.500000] random: crng init done\r
ES_HOSTKEY GEN_BEGIN\r
ES_HOSTKEY GEN_OK\r
ES_HOSTKEY NONEMPTY_OK\r
ES_HOSTKEY INETD_START\r
"""


def run(data: bytes, *args: str) -> tuple[int, str]:
    with tempfile.TemporaryDirectory() as tmp:
        path = Path(tmp) / "boot.bin"
        path.write_bytes(data)
        out = io.StringIO()
        with redirect_stdout(out):
            rc = cbl.main([*args, str(path)])
    return rc, out.getvalue()


def expect(cond: bool, what: str) -> None:
    if not cond:
        raise SystemExit(f"FAIL: {what}")


rc, out = run(GOOD)
expect(rc == 0 and "FAIL" not in out, f"good boot passes\n{out}")
expect("kernel time 14.5 s" in out, f"SSH ready time is reported\n{out}")

rc, out = run(GOOD, "--after-reboot")
expect(rc == 0, f"empty capsule passes after reboot\n{out}")

rc, out = run(GOOD.replace(b"GEN_OK", b"GEN_FAIL"))
expect(rc == 1 and "FAIL: no init marker failure: ES_HOSTKEY GEN_FAIL" in out,
       f"GEN_FAIL is caught\n{out}")

rc, out = run(GOOD + b"sh: pidof: applet not found\r\n")
expect(rc == 1 and "missing applet" in out, f"missing applet is caught\n{out}")

rc, out = run(GOOD.replace(b"ES_HOSTKEY INETD_START\r\n", b""))
expect(rc == 1 and "FAIL: ssh started" in out, f"missing SSH start is caught\n{out}")

capsule = GOOD.replace(b"CRASH_CAPSULE empty/invalid",
                       b"CRASH_CAPSULE VALID seq=3 reason=1 captured_cpu=0")
rc, _ = run(capsule)
expect(rc == 0, "a capsule alone does not fail a cold boot")
rc, out = run(capsule, "--after-reboot")
expect(rc == 1 and "FAIL: no crash capsule after reboot" in out,
       f"capsule after reboot is caught\n{out}")

prot = b"easystick-boot: C68 CPU1 PMA/PMP diff=%s\r\n"
rc, out = run(prot % b"00000000" + GOOD)
expect(rc == 0, f"CPU1 with CPU0's PMA/PMP passes\n{out}")
rc, out = run(prot % b"20000000" + GOOD)
expect(rc == 1 and "FAIL: no CPU1 protection mismatch" in out,
       f"a CPU1 PMA/PMP mismatch is caught\n{out}")

rc, out = run(GOOD + b"M3-lab: CONFIG ERROR: country=xx1 is not a two-letter code; ignored\r\n")
expect(rc == 0 and "WARN: config error" in out, f"config errors only warn\n{out}")

print("PASS: check-boot-log flags failures and passes a clean boot")
