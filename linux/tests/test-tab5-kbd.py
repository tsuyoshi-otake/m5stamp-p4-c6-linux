#!/usr/bin/env python3
"""Run the Tab5 keyboard driver on the host against a model of its MCU.

The driver source is taken out of linux/tab5/0098-keyboard-tab5-stm32.patch,
built with the stand-in kernel header in linux/tests/tab5-kbd/ and run
through the scenarios in harness.c: every key's character, the Aa and sym
keys, keys typed overlapping, attach, detach, an MCU restart and the LED.
It runs at three tick rates because the Aa and LED timing counts jiffies.

Needs a C compiler and the Linux input header.  Without them the test is
skipped, except in CI where that is a failure.
"""
import os
import shutil
import subprocess
import sys
import tempfile

ROOT = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
PATCH = os.path.join(ROOT, "linux", "tab5", "0098-keyboard-tab5-stm32.patch")
STUB = os.path.join(ROOT, "linux", "tests", "tab5-kbd")
DRIVER = "drivers/input/keyboard/tab5-kbd.c"
HEADERS = ("bits", "i2c", "input", "io", "jiffies", "module", "of", "workqueue")
TICK_RATES = (100, 250, 1000)


def driver_source():
    """The new file's lines from the patch, without their leading '+'."""
    with open(PATCH, encoding="utf-8", newline="") as f:
        lines = f.read().split("\n")
    start = lines.index("+++ b/" + DRIVER)
    header = lines[start + 1]
    if not header.startswith("@@ -0,0 +1,"):
        sys.exit(f"FAIL: unexpected hunk header for {DRIVER}: {header}")
    count = int(header.split("+1,")[1].split(" ")[0])
    body = lines[start + 2:start + 2 + count]
    if len(body) != count or any(not line.startswith("+") for line in body):
        sys.exit(f"FAIL: {DRIVER} hunk is not {count} added lines")
    return "\n".join(line[1:] for line in body) + "\n"


def unavailable(reason):
    if os.environ.get("CI"):
        sys.exit(f"FAIL: {reason}")
    print(f"SKIP: {reason}")
    sys.exit(0)


def main():
    source = driver_source()
    for name in HEADERS:
        if f"#include <linux/{name}.h>" not in source:
            sys.exit(f"FAIL: the stand-in header list names {name}.h, "
                     "which the driver does not include")
    included = source.count("#include <")
    if included != len(HEADERS):
        sys.exit(f"FAIL: the driver includes {included} headers, the "
                 f"stand-in covers {len(HEADERS)}; update HEADERS")

    cc = shutil.which(os.environ.get("CC", "cc")) or shutil.which("gcc")
    if not cc:
        unavailable("no C compiler on this host")

    with tempfile.TemporaryDirectory() as tmp:
        os.mkdir(os.path.join(tmp, "linux"))
        for name in HEADERS:
            with open(os.path.join(tmp, "linux", name + ".h"), "w") as f:
                f.write('#include "kstub.h"\n')
        with open(os.path.join(tmp, "tab5-kbd.c"), "w", newline="\n") as f:
            f.write(source)
        probe = subprocess.run(
            [cc, "-fsyntax-only", "-x", "c", "-"],
            input="#include <linux/input-event-codes.h>\n",
            capture_output=True, text=True)
        if probe.returncode:
            unavailable("no <linux/input-event-codes.h> on this host")

        for hz in TICK_RATES:
            exe = os.path.join(tmp, f"harness-{hz}")
            build = subprocess.run(
                [cc, "-std=gnu11", "-O1", "-Wall", "-Werror",
                 f"-DHZ={hz}", "-I", tmp, "-I", STUB,
                 os.path.join(STUB, "harness.c"), "-o", exe],
                capture_output=True, text=True)
            if build.returncode:
                sys.exit("FAIL: the driver does not build on the host\n"
                         + build.stdout + build.stderr)
            run = subprocess.run([exe], capture_output=True, text=True)
            if run.returncode:
                sys.exit(f"FAIL: keyboard scenarios at HZ={hz}\n"
                         + run.stdout + run.stderr)

    rates = ", ".join(str(hz) for hz in TICK_RATES)
    print(f"PASS: Tab5 keyboard driver scenarios at HZ {rates}")


if __name__ == "__main__":
    main()
