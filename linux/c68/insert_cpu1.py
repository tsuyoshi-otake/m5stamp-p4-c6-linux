#!/usr/bin/env python3
"""Insert the C68 CPU1 and optional IPI nodes into a staged UP DTS."""
from pathlib import Path
import re
import sys


def main() -> int:
    if len(sys.argv) not in (3, 4):
        print("usage: insert_cpu1.py <dts> <cpu1-inc> [<ipi-inc>]", file=sys.stderr)
        return 2
    dts_path = Path(sys.argv[1])
    inc_path = Path(sys.argv[2])
    ipi_path = Path(sys.argv[3]) if len(sys.argv) == 4 else None
    text = dts_path.read_text(encoding="utf-8")
    if "cpu@1" in text:
        print("C68 DTS gate failed: cpu@1 already present before insert", file=sys.stderr)
        return 1
    if ipi_path is not None and "espressif,esp32p4-ipi" in text:
        print("C68 DTS gate failed: IPI node already present before insert", file=sys.stderr)
        return 1
    marker = "CPU0_intc: interrupt-controller"
    idx = text.find(marker)
    if idx < 0:
        print("C68 DTS gate failed: CPU0_intc not found", file=sys.stderr)
        return 1
    # Close of cpu@0: first "};\n" at the cpu indent after CPU0_intc.
    rest = text[idx:]
    m = re.search(r"\n(\t{2})\};\n", rest)
    if not m:
        print("C68 DTS gate failed: cpu@0 closer not found", file=sys.stderr)
        return 1
    insert_at = idx + m.end()
    node = inc_path.read_text(encoding="utf-8")
    if not node.endswith("\n"):
        node += "\n"
    dts_path.write_text(text[:insert_at] + "\n" + node + text[insert_at:], encoding="utf-8")
    out = dts_path.read_text(encoding="utf-8")
    if out.count("cpu@1") != 1 or "CPU1_intc" not in out:
        print("C68 DTS gate failed: cpu@1 insert did not land", file=sys.stderr)
        return 1
    if ipi_path is not None:
        soc_marker = "soc: soc {"
        soc_idx = out.find(soc_marker)
        if soc_idx < 0:
            print("C68 DTS gate failed: soc node not found", file=sys.stderr)
            return 1
        rest = out[soc_idx:]
        m = re.search(r"\n(\t)\};\n", rest)
        if not m:
            print("C68 DTS gate failed: soc closer not found", file=sys.stderr)
            return 1
        ipi_node = ipi_path.read_text(encoding="utf-8")
        if not ipi_node.endswith("\n"):
            ipi_node += "\n"
        soc_insert_at = soc_idx + m.start() + 1
        out = out[:soc_insert_at] + ipi_node + out[soc_insert_at:]
        dts_path.write_text(out, encoding="utf-8")
        out = dts_path.read_text(encoding="utf-8")
        if out.count("espressif,esp32p4-ipi") != 1:
            print("C68 DTS gate failed: IPI node insert did not land", file=sys.stderr)
            return 1
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
