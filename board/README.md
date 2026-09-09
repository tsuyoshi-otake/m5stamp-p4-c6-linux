# Stamp-P4 board port notes

EasyStick Rev0.15 is an **optional USB mass-storage carrier**, not a requirement
for Linux boot, Wi-Fi, SSH, or the Stamp-P4 USB-C serial console. It makes the
GPIO26/27 USB configuration drive accessible through USB-A. Schematics, PCB
Gerbers, and assembly notes are available in [`hardware/`](hardware/README.md)
under the MIT License. The project owner reports successful manufacture and use.

This directory intentionally contains no buildable DTS yet. A Linux DTS must
be authored from the locked kernel and the measured EasyStick module/flash
configuration; copying the WHY2025 badge DTS would silently assign the wrong
UART, storage, GPIO, and C6 transport.

Before adding `stamp-p4.dts`, M0 must provide or confirm:

- module revision and actual flash/PSRAM identification;
- boot UART pins and a captured ROM/boot log;
- measured flash partition boundaries that fit 16 MiB without overlap;
- C6 AddOn SDIO reset/handshake behavior;
- watchdog/reset behavior after a failed kernel or C6 image.

The carrier-level facts that are safe to consume from scripts are in
[`../board-contract.json`](../board-contract.json) and are checked against the
Rev0.15 netlist by `../tools/verify-board-contract.py`. The netlist is a
separately managed hardware-design export, so provide it explicitly; its
SHA-256 must match the value pinned in the contract:

```bash
python3 ../tools/verify-board-contract.py \
  --netlist /absolute/path/to/netlist.rev0.15.json
```
