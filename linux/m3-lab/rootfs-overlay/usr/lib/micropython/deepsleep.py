# Deep Sleep from MicroPython (#56).  The board reboots into Deep Sleep and
# starts Linux (and the autorun program) again when it wakes.
#
#   import deepsleep
#   n = deepsleep.memory()            # bytes kept across Deep Sleep
#   deepsleep.memory(b"count=%d" % k) # up to 1024 bytes
#   deepsleep.wake_reason()           # "timer", "gpio:4", "poweron", "reset"
#   deepsleep.sleep(60000)            # ms; does not return
#   deepsleep.sleep(gpio={4: 0})      # wake when GPIO4 reads 0
#
# sleep() runs the deepsleep command, which waits for the USB drive to be
# saved and does not sleep in a boot where BOOT was held.
import os
import time

_SYS = "/sys/kernel/board/"


def _read(name):
    try:
        with open(_SYS + name) as f:
            return f.read()
    except OSError:
        return ""


def wake_reason():
    return _read("wake_reason").strip() or "unknown"


def wake_info():
    d = {}
    for line in _read("wake_info").split("\n"):
        k, _, v = line.partition("=")
        if k:
            d[k] = v
    return d


def memory(data=None):
    if data is None:
        try:
            with open(_SYS + "sleep_memory", "rb") as f:
                return f.read()
        except OSError:
            return b""
    if len(data) > 1024:
        raise ValueError("sleep memory holds 1024 bytes")
    with open(_SYS + "sleep_memory", "wb") as f:
        f.write(data)


def sleep(ms=0, gpio=None, pull=True, force=False):
    cmd = "deepsleep"
    if force:
        cmd += " -f"
    if not pull:
        cmd += " --no-pull"
    for pin, level in (gpio or {}).items():
        cmd += " --gpio %d=%d" % (int(pin), 1 if level else 0)
    if ms:
        cmd += " %dms" % int(ms)
    if os.system(cmd) != 0:
        raise OSError("deepsleep refused")
    # The reboot is under way; do not carry on with the program.
    while True:
        time.sleep(60)
