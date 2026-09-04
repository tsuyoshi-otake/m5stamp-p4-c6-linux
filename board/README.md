# Stamp-P4 board port notes

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
Rev0.15 netlist by `../tools/verify-board-contract.py`.
