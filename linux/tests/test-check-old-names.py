#!/usr/bin/env python3
"""Host test for check-old-names.py (#55b guard).

- The checker passes on this tree (exit 0).
- Its allowlist accepts exactly the #55b exceptions and rejects
  everything else (unit-level checks on the compiled pattern and the
  region helper).
"""
import os
import subprocess
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.abspath(os.path.join(HERE, os.pardir, os.pardir))
CHECKER = os.path.join(HERE, "check-old-names.py")

sys.path.insert(0, HERE)
import importlib.util
spec = importlib.util.spec_from_file_location("check_old_names", CHECKER)
mod = importlib.util.module_from_spec(spec)
spec.loader.exec_module(mod)

fails = []


def check(name, cond):
    print(("PASS" if cond else "FAIL"), name)
    if not cond:
        fails.append(name)


# 1. live run on the repo root
r = subprocess.run([sys.executable, CHECKER, ROOT],
                   capture_output=True, text=True)
check("checker exit 0", r.returncode == 0)
check("checker pass banner", "no 'easystick' references" in (r.stdout or ""))

# 2. allowlist: exceptions accepted, everything else rejected
ap = mod.ALLOWED_PATH
for path in ("vendor/esp-idf/x", "board/hardware/README.md",
             "board/hardware/rev0.15-wip/SHA256SUMS.txt",
             "linux/tests/issue55-rename-2026-10-23.md",
             "linux/tests/check-old-names.py"):
    check(f"allowed {path}", bool(ap.search(path)))
for path in ("linux/tests/v0.4.1-hardware-validation-2026-10-21.md",
             "linux/tests/anything-2026-08-10.md", "tests/x-2026-08-10.md",
             "docs/troubleshooting.md", "README.md", "linux/build-m1.sh",
             "linux/kernel-patches/0076-nommu-pool-sysfs.patch"):
    check(f"rejected {path}", not ap.search(path))

# 3. region helper: '55b keep' lines allowed; '#55 compat' no longer is
import tempfile
with tempfile.TemporaryDirectory() as td:
    p = os.path.join(td, "f.txt")
    with open(p, "w") as f:
        f.write("nope\n#55 compat\neasystick x\n#55 compat end\n"
                "no2\n55b keep\neasystick y\n55b keep end\nno3\n")
    ok = mod.allowed_lines(p)
check("keep lines allowed, compat not", ok == {6, 7, 8})

sys.exit(1 if fails else 0)
