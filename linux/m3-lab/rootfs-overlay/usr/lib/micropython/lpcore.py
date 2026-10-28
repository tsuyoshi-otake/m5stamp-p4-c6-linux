# The user LP core program from MicroPython (#59).  lp_core= and lp_program=
# in config.txt load it at boot; this module reads and writes its variables.
#
#   import lpcore
#   lpcore.state()                 # {"state": "running", "mode": "always", ...}
#   lpcore.read(0x50108a40)        # the 32-bit variable at that address
#   lpcore.read(0x50108a40, 8)     # 8 bytes from there
#   lpcore.write(0x50108a40, 5)    # root only: a 32-bit value, or bytes
#
# Addresses are the ones in the program's ulp_main.ld (PROVIDE(ulp_x = ...))
# or from nm on ulp_main.elf.  Offsets into the LP RAM reserve work too.
_DIR = "/sys/kernel/board/lp_core/"
_base = None


def state():
    d = {}
    try:
        with open(_DIR + "state") as f:
            for line in f.read().split("\n"):
                k, _, v = line.partition("=")
                if k:
                    d[k] = v
    except OSError:
        pass
    return d


def _offset(addr, n):
    global _base
    if _base is None:
        s = state()
        if "ram_base" not in s:
            raise OSError("no LP core support")
        _base = (int(s["ram_base"], 16), int(s["ram_bytes"]))
    base, size = _base
    off = addr - base if addr >= base else addr
    if off < 0 or off + n > size:
        raise ValueError("address 0x%x is outside the LP RAM reserve" % addr)
    return off


def read(addr, n=None):
    off = _offset(addr, n or 4)
    with open(_DIR + "memory", "rb") as f:
        f.seek(off)
        b = f.read(n or 4)
    return b if n else int.from_bytes(b, "little")


def write(addr, value):
    if isinstance(value, int):
        value = (value & 0xFFFFFFFF).to_bytes(4, "little")
    off = _offset(addr, len(value))
    with open(_DIR + "memory", "r+b") as f:
        f.seek(off)
        f.write(value)
