# Native Linux integration boundary

This directory contains EasyStick-owned Linux integration: the Stamp-P4 DTS,
kernel configuration, Buildroot external tree, rootfs overlay, image-size
checks, and boot-shim handoff metadata. The pinned upstream source trees are
included as gitlinks in [`../vendor/`](../vendor/); generated build output and
toolchains must remain external.

The immutable upstream inputs live one level up in
[`../versions.lock.json`](../versions.lock.json), and their submodule pins are
listed in [`../vendor/README.md`](../vendor/README.md). The carrier facts that a
future DTS may consume are in [`../board-contract.json`](../board-contract.json)
and are checked against the Rev0.15 netlist before implementation changes.

The M1 artifact specification is now present, and a larger M2 candidate was
flashed to the documented hand-assembled target. It reaches `/sbin/init` and
Linux reports `mmc0: new SDIO card at address 0001`; see
[`tests/m2-sdio-boot-2026-08-10.md`](tests/m2-sdio-boot-2026-08-10.md). An
earlier isolated capture reached the ESP-Hosted boot event and created
`wlan0`, and for several days that result was not reproducible: controlled
reboots of the lab image and of what was believed to be the same candidate
showed `OPEN_DATA_PATH ret=0`, `Rx Pos=0`, `BOOT_CMD53_RX len=0`, and only
`lo`. **That is now explained and reproducible on demand** — see the A0 restore
below. Those intermediate reboots were not in fact the same candidate: every
one of them carried a rootfs whose `esp32_sdio.ko` had changed. M2 remains
**not accepted at present**, because `wlan0` has so far been recovered only by
restoring the preserved historical artifact set, not on the current stack. Keep
the C6 image unchanged before reflashing C6 or beginning DHCP/SSH acceptance.
Wi-Fi credentials remain operator-supplied outside Git. The `flash-layout.json`
map remains a candidate with a hard write gate, and `tools/verify-images.py`
checks bounds, alignment, overlap, and artifact sizes without writing a device.
USB gadget and SSH remain later acceptance gates.

The later aggregate-receive experiment and the current controlled reboot are
recorded in
[`tests/m3-lab-sdio-aggregate-2026-08-10.md`](tests/m3-lab-sdio-aggregate-2026-08-10.md).
They pass the P4 flash gate, Linux boot, and SDIO card discovery, but do not
reproduce the pending C6 boot frame (`wlan0` is absent). Treat all such images
as diagnostic only; the earlier reset-before-enumeration capture is retained
as historical evidence, not as a release or acceptance baseline, until the
lifecycle difference is explained.

The 2026-08-11 on-board telemetry is recorded in
[`tests/m2-sdio-boot-2026-08-10.md`](tests/m2-sdio-boot-2026-08-10.md). After
the requested Type-C power cycle, GPIO42 was High/enabled, GPIO46/DAT1 was
High, and the Slot 1 input matrix matched the ESP-IDF card-interrupt setup.
The DW-MMC raw Slot 1 interrupt bit remained set while masked status was clear;
the ESP-Hosted `INT_ST` register and RX length both remained zero. A single
runtime GPIO42 reset followed by a Linux reboot did not produce a pending
boot frame or `wlan0`. This is still P4-side diagnostic evidence, not an M2
acceptance result. Keep the C6 image unchanged and capture raw/masked DW-MMC
state around `OPEN_DATA_PATH`/CMD53 (or use a logic analyzer) before any C6
reflash.

**The regression is host-side and its cause is identified.** Restoring the six
preserved last-good P4 flash regions verbatim and cold-booting restored the
complete historical build-#29 behaviour: `Tx Pos = 10`, `BOOT_CMD53_RX: len=40`,
the ESP boot-up event, `ESP-Hosted Version: NG-1.0.6.0.1`, and `wlan0`. The
historical secondary command timeouts returned with it, which is what shows the
restore reproduced that state faithfully rather than recovering part of it by
accident. Comparing the last-good and first-bad rootfs images per file then
reduced the functional delta to a single object, `esp32_sdio.ko`: init scripts,
module-loading behaviour and Buildroot package contents are byte-identical. The
mechanism is a register-access refactor in `esp_sdio_api.c` — `esp_read_reg()`
and `esp_write_reg()` changed from byte-wise `sdio_readb`/`sdio_writeb` (CMD52)
loops to a size dispatch, so every 4-byte slave-register access became one CMD53
`IO_RW_EXTENDED` transfer, and that form reads back zero on this host. That
single change accounts for `Tx Pos = 0`, `INT_ST = 0`, `PACKET_LEN = 0` and the
missing boot frame — each of which the 0012 A/B below restores by reverting it.
It does **not** by itself account for the missing `wlan0`, which stays absent
after the revert. Full evidence, hashes and the provenance finding are in
[`tests/a0-lastgood-restore-and-rootfs-bisect-2026-08-12.md`](tests/a0-lastgood-restore-and-rootfs-bisect-2026-08-12.md),
with the preceding narrowing in
[`tests/wlan0-regression-bisect-2026-08-12.md`](tests/wlan0-regression-bisect-2026-08-12.md).
This supersedes the logic-analyzer and C6-reflash guidance above as the next
step, and it removes the C6 from suspicion for this symptom.

`0012-easystick-sdio-register-cmd52-access.patch` reconstructs the byte-wise
register accessors from the retained module binary's relocations, because no
commit ever contained them — they were an uncommitted edit to the vendored
`esp-hosted` working tree, destroyed by a forced submodule checkout. The CMD53
**block/data** path in `esp_read_block`/`esp_write_block` is deliberately left
on `sdio_memcpy_fromio`/`toio`, since the A0 capture shows that path carrying
live traffic. 0012 has since been built into the current
`--profile m2` stack, and the built `esp32_sdio.ko` was measured rather than
assumed: `esp_read_reg` reaches `sdio_readb` and has no `sdio_memcpy_fromio`
call site, while `esp_read_block`/`esp_write_block` stay byte-identical to the
last-good #29 module. That candidate is written to the P4 and passed
byte-for-byte verification, with the C6 untouched.

**Its hardware result is now captured and classified: 0012 fixes the register path
and does not restore `wlan0`.** The critical expected transition happened —
`Tx Pos` went from `0` to `10`, and all four token values now match the last-good
#29 boot exactly (`0 / 0 / 0 / 10`), with the length register reading 40 where
every failing run read 0. `OPEN_DATA_PATH ret=0` and `BOOT_CMD53_RX: len=40` both
pass. But `Received ESP boot-up event`, `ESP-Hosted Version` and `wlan0` are all
still absent, because the 40-byte frame's payload reads back as one 4-byte unit
repeated (`03 00 00 00 …`) where #29 streams a real header ending in ASCII `NG`;
the driver drops it as `len=3 offset=0`. The `-22` probe failure is **not** a
symptom — #29 produced the identical `-22` and created `wlan0` anyway. So the
register-access mechanism is experimentally confirmed for the token and length
values and **not** confirmed end to end, and at least one further difference from
#29 remains. Two of those differences are the diagnostics themselves: #29 lacks
0010, whose probe-time `BOOT_POLL` reads the length register and performs a manual
receive before the ISR path runs, and lacks 0011's interrupt acknowledge. Both are
candidate confounds and both stay in place. No second patch was stacked on 0012. Full classification, the side-by-side payload comparison and
the open questions are in §9.8 of
[`tests/a0-lastgood-restore-and-rootfs-bisect-2026-08-12.md`](tests/a0-lastgood-restore-and-rootfs-bisect-2026-08-12.md).

The official Stamp-P4 ESP-Hosted reference was compared before the next
diagnostic image. M5Stack's [`M5STACK_StampP4/sdkconfig.esp_hosted`](https://github.com/m5stack/uiflow-micropython/blob/master/m5stack/boards/M5STACK_StampP4/sdkconfig.esp_hosted)
selects SDIO Slot 1, 4-bit mode, GPIO43/44/45..48, GPIO42, active-high reset,
and a 1500 ms post-reset delay. The official Stamp-P4 Wi-Fi example uses the
same pins and reaches STA/DHCP in its supported Arduino environment. The
Espressif ESP-Hosted-MCU implementation documents the corresponding sequence
as active-high for 10 ms, inactive for 10 ms, active again, then a 1500 ms
readiness wait before card initialization. EasyStick's prior shim used
1 ms/20 ms/100 ms, so the next P4-only candidate now matches the official
10 ms/10 ms/1500 ms timing. This aligns the board lifecycle only; it does not
yet prove Linux SDIO IRQ/CMD53 compatibility or accept M2.

The `c66451c` candidate was subsequently written to the P4 only and passed
byte-for-byte verification for bootloader, shim, Image, rootfs, and DTB. A
single reset/capture with the C6 image unchanged logged the new 1500 ms
readiness sequence, SDIO card discovery, `OPEN_DATA_PATH ret=0`, and repeated
zero-status IRQ callbacks, but still had `Rx Pos=0`, `BOOT_CMD53_RX len=0`, no
ESP boot-up event, and no `wlan0`. The candidate therefore rules out this reset
timing change as a sufficient fix; keep M2 in diagnostic status and continue
with P4-side pending-frame/IRQ/CMD53 telemetry.

The follow-up PIO diagnostic also found and corrected a DTS omission: kernel
patch `0008-easystick-dw-mmc-force-pio.patch` only takes effect when the
SDIO node contains `easystick,force-pio`. The first `c66451c` boot reported
`Using internal DMA controller`; the PIO candidate now reports `Using PIO
mode`. PIO still produced `Rx Pos=0`, `BOOT_CMD53_RX len=0`, no ESP boot-up
event, and no `wlan0`, so DMA selection was a real test-condition defect but
not the remaining boot-frame cause.

The SSH milestone is intentionally separate from the first boot image. The
M3 profile in [`m3/`](m3/) adds Buildroot's small Dropbear server, BusyBox
`inetd` for NOMMU, and compile-time password-authentication disablement. Once
the Stamp AddOn C6 SDIO path has produced a stable `wlan0`, enable that
profile with a validated writable `/config` overlay. Keep host keys and
network configuration there; never commit credentials or private keys, and
re-check the flash/PSRAM size budget before enabling it.

The same M3 profile documents the USB initial-setup contract in
[`m3/initial-setup.md`](m3/initial-setup.md). Both connectors use the same
`/usr/sbin/easystick-firstboot` flow once their local login console is
available: USB-C is the intended primary path (with the boot-shim/UART
fallback until native Linux USB-Serial/JTAG support exists), and USB-A is the
fallback after the separate M4 CDC-ACM gadget work is complete. Neither path
turns the carrier into a USB host or supplies VBUS.

The M1 bring-up fixture intentionally contains the requested local
`pi`/`raspberry` bootstrap; change it immediately and do not expose it on a
network. The M3 profile replaces that fixture with a locked `p4` account and
key-only Dropbear: password and root SSH login stay disabled.

The C68-CLEAN-RELEASE secondary-hart contract remains recorded as
**L2-v2 / L2-HW**. The dedicated 0045+0047+0048+0049 `m5stamp` smoke profile
also produced one clean 120 s **L3 smoke PASS** with two online CPUs,
CPU1-affine work, and bidirectional IPI completion; an immediately preceding
same-hash attempt hit a transient WDT before sending the helper, and a later
pre-mitigation stress image faulted during startup before command dispatch.
The corrected seedrng-free, bounded-top `m1-topcapfix` image then passed three
consecutive same-input stress captures. The versioned pass/fail contract,
hashed artifacts, and the rebuild-gate gap are in
[`tests/c68-l2-hw-contract-2026-08-18.md`](tests/c68-l2-hw-contract-2026-08-18.md).
C6/SDIO/SSH work stays on the `CONFIG_SMP=n` UP profile until that C68
profile is fail-closed and, for a merged default image, until broader L3
coverage and the remaining release gates are closed.

The corrected isolated C68 stress image built with
`EASYSTICK_C68_L3_SMOKE=1 EASYSTICK_C68_STRESS=1` passed three consecutive
120 s UART captures: BusyBox `top`, `/usr/sbin/m5stamp-smp-stress`, CPU1-affine
processes, repeated fork/exec, bidirectional IPI calls, and the final
`M5STAMP_STRESS PASS` were all observed in each run. The raw captures, schema-v2
JSON files, and exact four flash-input hashes are retained in
[`tests/c68-l2-hw-contract-2026-08-18.md`](tests/c68-l2-hw-contract-2026-08-18.md).
The older `m1-isolated-v2` image's seedrng/read-only startup fault remains
retained as historical negative evidence; it predates the seedrng removal and
is not an input to the corrected pair. The default capture runs the helper for
90 s inside the 120 s UART window so its worker-stop, final `top`, and PASS
markers are observable. Broader soak and the UP profile remain unchanged.
Buildroot profile identity is stamped in the output and the final rootfs
gate rejects stale Dropbear, network, smoke-helper, or stress-helper files
when switching back to a smaller profile.

Temperature support remains a separate, opt-in Phase-1 feature. The
independent boot-shim oracle has a recorded hardware PASS in
[`tests/tsens-oracle-2026-08-19.md`](tests/tsens-oracle-2026-08-19.md).
`EASYSTICK_TSENS_LINUX=1` selects kernel patch 0050, which configures the
ESP32-P4 REGI2C TSENS DAC in Linux, verifies `7 -> 7` and `15 -> 15`
readback, decodes the 10-bit eFuse field using ESP-IDF's bit-8-sign /
low-8-magnitude rule, and registers an `esp32p4-tsens` thermal zone. The Linux
hardware probe passed in the first v6 capture, including oracle cleanup,
`/sys/class/thermal` registration, and the same-image 90-second SMP stress.
The regenerated v7 quartet then passed three consecutive 90-second SMP
regressions with stress-side temperature aggregation (`temp_failures=0` in
all three); the initial pre-rebuild v7 watchdog capture remains documented as
negative evidence in
[`tests/tsens-linux-2026-08-19.md`](tests/tsens-linux-2026-08-19.md). This does
not change the default `CONFIG_SMP=n` profile.

Build C68 with `linux/build-c68.sh` (not `build-m1.sh` alone). The UP DTS
and `linux.config` stay single-hart; C68 injects `c68/cpu1.dts.inc`,
`c68/ipi.dts.inc`, and `c68/linux.config.fragment` at build time. The A'
provider is staged by patches 0040/0041; its first gate is publication of the
generic IPI virq range before the CPU1 `riscv_ipi_enable()` call. Patch 0042
adds a target1/source54 SYSTIMER clockevent for CPU1; the timer gate is
separate from the IPI gate and requires real per-hart delivery.

The source DTS is a project-owned copy and must be compiled against the
locked Linux tree after the ESP32-P4 baseline patches have been reviewed and
applied. The build entry point materializes it as a minimal `espressif/`
overlay with its own `Makefile`, as required by Linux 6.12+, while keeping the
single source under `dts/` in this repository. It is not the WHY2025 badge DTS.
The base M1 image remains earlycon/UART-oriented; C68 top/stress profiles
explicitly enable the board-local USB Serial/JTAG ACM driver and the
`ttyGS1` askfirst shell used by the COM10 capture. The same entry point stages
`linux.config` as an explicit Buildroot kernel fragment and verifies the
resulting 32-bit NOMMU/FLAT configuration before reporting success.

Run the static map check from this directory:

```text
python3 tools/verify-images.py --layout flash-layout.json
```
