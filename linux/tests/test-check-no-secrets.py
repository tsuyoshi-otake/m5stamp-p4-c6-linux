#!/usr/bin/env python3
"""Self-test for tools/check-no-secrets.py with synthetic trees."""

import importlib.util
import io
import tempfile
from contextlib import redirect_stdout
from pathlib import Path

HERE = Path(__file__).resolve().parent
spec = importlib.util.spec_from_file_location(
    "check_no_secrets", HERE.parents[1] / "tools" / "check-no-secrets.py")
cns = importlib.util.module_from_spec(spec)
spec.loader.exec_module(cns)


def run(files: dict[str, str]) -> tuple[int, str]:
    with tempfile.TemporaryDirectory() as tmp:
        root = Path(tmp)
        for name, text in files.items():
            path = root / name
            path.parent.mkdir(parents=True, exist_ok=True)
            path.write_text(text, encoding="utf-8")
        out = io.StringIO()
        with redirect_stdout(out):
            rc = cns.main(["--root", str(root)])
    return rc, out.getvalue()


def expect(cond: bool, what: str) -> None:
    if not cond:
        raise SystemExit(f"FAIL: {what}")


rc, out = run({"notes.txt": "nothing to see\n"})
expect(rc == 0 and "PASS" in out, f"clean tree passes\n{out}")

rc, out = run({"linux/tests/test-x.sh": "wifi_psk=secret123\n"})
expect(rc == 0, f"dummy value under linux/tests passes\n{out}")

rc, out = run({"config.txt": "wifi_psk=secret123\n"})
expect(rc == 1, f"dummy value outside linux/tests fails\n{out}")

rc, out = run({"linux/tests/test-x.sh": "wifi_psk=actual-passphrase\n"})
expect(rc == 1, f"real-looking value under linux/tests fails\n{out}")

rc, out = run({"linux/tests/test-x.sh": "wifi_psk=secret123\nwifi_psk=real1\n"})
expect(rc == 1, f"a real value among dummies fails\n{out}")

rc, out = run({"linux/tests/test-x.sh": "-----BEGIN " "OPENSSH KEY-----\n"})
expect(rc == 1, f"key material under linux/tests fails\n{out}")

rc, out = run({"vendor/sub/key.pem": "-----BEGIN " "RSA KEY-----\n"})
expect(rc == 1, f"key material outside linux/tests fails\n{out}")

out = io.StringIO()
with redirect_stdout(out):
    rc = cns.main([])
expect(rc == 0, f"the real tree passes\n{out.getvalue()}")

print("PASS: check-no-secrets flags real values and ignores test dummies")
