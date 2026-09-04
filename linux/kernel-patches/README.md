# M1 kernel patch contract

The locked `why2025_linux_reference` checkout is a reference input, not a
board image and not a source tree to vendor. M1 needs the following reviewed
P4 baseline functionality before `linux.config` can be applied:

1. ESP32-P4 RV32/NOMMU architecture, CLIC, SYSTIMER, cache operations, and
   `esp32_uart` (`0001-riscv-esp32p4-baseline.patch` in the reference).
2. The ESP32-P4 watchdog driver (`0019-watchdog-esp32p4-mwdt.patch`) for
   deterministic recovery.
3. The SYSTIMER level-trigger fix and signal/interrupt return hardening
   required by the P4 CLIC erratum (`0022`, `0023`, and `0031` in the
   reference), subject to a board-port review and explicit known-issues entry.

The reference patch files are fetched externally using `versions.lock.json`.
They are not copied into this repository unchanged. Before a flashable M1
release, the build script must materialize a reproducible, reviewed patch
series in an external worktree, record each source hash, and add any
EasyStick-specific deltas here. The Stamp-P4 DTS in `../dts/` is the only
board description allowed; the WHY2025 DTS and its SPI C6 nodes must not be
applied.

`0022-esp32p4-systimer-level-trigger.patch` is an EasyStick-owned extraction
of the driver hunk from the reference `0022` patch. The reference file also
edits the WHY2025 DTS, which is intentionally not applied; the board-owned
DTS in `../dts/` already selects `IRQ_TYPE_LEVEL_HIGH`.

`0035-easystick-esp32p4-wdt-feed-observe.patch` is m3-lab-only observe
telemetry on `esp32p4_wdt_ping`: it prints `ES_WDT WDT_FEED_COUNT` and
`last_feed_jiffies` without changing timeout, stage policy, or feed rate.

`0036-easystick-tcp22-bidirectional-ledger.patch` is m3-lab-only observe
telemetry for TCP port 22 (`ES_TCP` markers on sendmsg/push/write_xmit,
ip_queue_xmit, dev_queue_xmit, ip_rcv, tcp_v4_rcv, established, data_queue,
and ACK build/xmit). It does not change TCP behaviour, Nagle, or sysctls.

`0037-easystick-c67-l2-stage-smpboot.patch` is enabled for C67-L2-STAGE
(`EASYSTICK_C67_SMP_STAGE=1`) and the C68 fail-closed profile. It adds
observe-only `pr_info` breadcrumbs in `arch/riscv/kernel/smpboot.c`:
`C67 UP`, `C67 START`, `C67 CALLIN`, `C67 ONLINE`, then after
`complete(&cpu_running)` `C67 COMPLETE`, immediately before
`cpu_startup_entry()` `C67 STARTUP`, and on CPU0 `__cpu_up()` success
return `C67 UP_RETURN`. It does not change CPU start policy, timeouts,
or IPI handling.

`0038-easystick-c68-clean-release-spinwait.patch` is a **template**. It
keeps `0xC68DEAD1/2` placeholders in Git. `linux/build-c68.sh` renders it
into `<output>/c68-rendered-patches` after extracting boot-shim `nm`
addresses. `build-m1.sh` with `EASYSTICK_C68_CLEAN_RELEASE=1` refuses the
kernel build unless that override directory is set, is not the patch
stage, and contains no `C68DEAD` bytes.

`0039-easystick-c68-clean-release-smpboot.patch` is rendered the same way.
It extends the C67 observe breadcrumbs with C68 timeout diagnostics in
`__cpu_up()` without duplicating the C67 marker set.

`0040-easystick-esp32p4-ipi-provider.patch` extends the ESP32-P4 CLIC
interrupt-matrix mapping to both HP harts and adds the `CPUHP` anchor used by
the provider. `0041-easystick-esp32p4-ipi-driver.patch` maps the
`CPU_INT_FROM_CPU_0/1` sources, multiplexes them into the generic RISC-V IPI
range, and calls `riscv_ipi_set_virq_range()` only after the CPU-starting hook
has been installed. The C68 fail-closed gate checks that both patches, the
provider symbol, and the IPI DTS node are present in the same staged triple.
The 2026-08-19 A' capture passed that virq-range gate in hardware:
`smp.c:176` was absent and `Brought up 1 node, 2 CPUs` was observed.

`0042-easystick-esp32p4-systimer-cpu1.patch` gives SYSTIMER target 1 and its
source-54 CLIC route to CPU1. It is the next implementation candidate because
the same capture still stalled before init with CPU1 timer delivery absent.
The first 0042 coherent triple was classified on hardware for 120 seconds.
The SYSTIMER registration line appeared and the A' IPI gate remained passing,
but the last C67 marker was `C67 UP_RETURN`: `C67 STARTUP`,
`Brought up 1 node, 2 CPUs`, and init were absent. No RCU stall appeared in
that window, so this is a new timer-startup failure, not L3 evidence.

The source-level cause was a target-local register trigger mistake. `COMP0_LOAD`
and `COMP1_LOAD` are separate write-one registers, so each requires bit 0;
the initial implementation wrote `BIT(cpu)` and therefore wrote `BIT(1)` to
`COMP1_LOAD`. The patch now writes `1`, and the C68 gate rejects the old form
before a replacement triple can be staged. The corrected patch must be
classified as another coherent triple; the initial result is not retroactive
evidence for L2.5 or L3.

`0045-easystick-esp32p4-systimer-st-conf-lock.patch` serializes the shared
`ST_CONF`/`ST_INT_ENA` programming sequence with a raw spinlock while
preserving the target-local write-one `COMP*_LOAD` operation. Its standalone
120 s hardware control still reached the CPU1 illegal-execution/RCU/CRED
failure, so it is not accepted as a complete fix by itself.

`0046-easystick-esp32p4-cpu1-context-observe.patch` is an observation-only
patch. It reports CPU1 illegal traps and sampled CPU1 context switches; it
does not change the hotplug, IPI, or timer policy. The first coherent
0045+0046 hardware shot reached `Run /sbin/init`, observed valid CPU1 context
switches, CPU1 timer IRQ delivery, and bidirectional IPIs without an illegal
trap, RCU stall, or panic. This is **L2.5 PASS for the observation triple**,
but not an L3 result and not proof that the observer fixed the race: its
`pr_info()` in `context_switch()` changes timing. The next control must retain
0045 while removing that printk.

`0047-easystick-esp32p4-cpu1-illegal-observe.patch` is that low-overhead
control: it retains only the CPU1 illegal-trap snapshot from 0046 and contains
no scheduler/context-switch printk. With 0045 and 0047 selected, the ordinary
path has no new log operation; the observer runs only if the illegal trap
recurs. Its staged hash and complete triple must be recorded before any
hardware classification.

`0048-easystick-esp32p4-l3-smoke-endpoint.patch` is selected only for the
explicit C68 L3 smoke profile. It creates the test-only
`/proc/m5stamp_smp_smoke` endpoint. A userspace write runs a bounded,
CPU-verified CPU0 -> CPU1 `smp_call_function_single()` and a non-blocking
CPU1 -> CPU0 reverse notification, then reports the callback CPU identities
and return codes. The reverse leg is intentionally non-blocking while the
CPU0 callback returns; waiting inside that callback would deadlock the CPU0
IPI handler. It does not alter the generic IPI provider, timer policy, or
scheduler path, and it is not included in ordinary C68 or M3 images.

`0049-easystick-esp32p4-l3-smoke-workqueue.patch` completes that reverse leg
without calling `smp_call_function_single()` from the CPU1 IPI callback.
The callback queues CPU1 work, and the work item performs the reverse call
from task context; the L3 gate also requires work cleanup before the result
is released. The result now advances a generation per run, rejects stale
work, records a busy enqueue as failure, and asserts that an old work item is
not still pending before a new run starts. This keeps a timed-out diagnostic
run from writing into a later result or completing its later wait, while also
keeping the endpoint test from generating the generic `kernel/smp.c`
interrupt-context warnings it is intended to diagnose.

`0050-thermal-esp32p4-add-lp-tsens-driver.patch` is an experimental, opt-in
Linux temperature-sensor driver. It is selected only with
`EASYSTICK_TSENS_LINUX=1` on the C68 build path, and is intentionally separate
from the SMP patches. The probe maps the P4 LP-TSENS, LP_PERI, LP analog-I2C
master, PMU, and eFuse windows; enables the analog-I2C path; resets TSENS;
performs the REGI2C DAC sequence `7 -> readback -> 15 -> readback`; then
enables sampling, waits 300 us, and registers a tripless `esp32p4-tsens`
thermal zone. `get_temp()` only reads TSENS output and uses integer
milli-degree conversion with the ESP-IDF 10-bit eFuse field's bit-8-sign /
low-8-magnitude correction (bit 9 is ignored). A successful Linux probe emits
one `P4_TSENS_LINUX PASS` line.
This is not yet a production or upstream-quality clock/reset/nvmem provider:
the bring-up node supplies named MMIO windows and the tree currently has no
other REGI2C consumer, which are explicit acceptance conditions for this
experiment. The first v6 hardware capture also showed the independent
boot-shim oracle passing and cleanup completing, with matching `efuse_raw=33`,
`delta_t=33`, `range_reg=15`, `dac_probe=7`, and `dac_readback=15`; final
three-run repeatability and stress-side temperature aggregation remain open.

C68 hardware evidence is **L2.5 PASS on the corrected 0045+0046
observation quadruple**: the LZ4 rootfs mounts, `esp32_sdio.ko` loads with
`6.18.35 SMP riscv`, and the prior version-magic warning is absent. The
first full quadruple was deliberately rejected because a manual repack used
XZ while the kernel has `CONFIG_SQUASHFS_XZ` disabled; the corrected image
uses Buildroot's LZ4/128 KiB/`-Xhc` settings. The 0045+0047+0048+0049
profile then produced one clean 120 s **L3 smoke PASS**:
`online=0-1`, CPU1-affine work, bidirectional reverse-IPI completion, and
`M5STAMP_L3SMOKE PASS`, with no `smp.c:176`, WDT, illegal-trap, warning, or
RCU-stall marker. The corrected seedrng-free, bounded-top stress image then
produced three consecutive same-input `helper-pass` captures with CPU1 work,
five bounded `top` snapshots per run, timer/IPI progress, and no fault
markers. The earlier pre-mitigation startup fault remains historical
evidence; no manufacturing or default-profile state is advanced. See
[`../tests/c68-l2-hw-contract-2026-08-18.md`](../tests/c68-l2-hw-contract-2026-08-18.md).
Committed `linux.config` remains `CONFIG_SMP=n` and the board DTS remains
`cpu@0` only. The C68 profile appends `linux/c68/linux.config.fragment` and
inserts `linux/c68/cpu1.dts.inc` at build time.

This directory documents a real dependency boundary while the series is
still under review. A clean build must fail early if the baseline symbols are
absent rather than silently dropping UART, watchdog, or NOMMU support during
`olddefconfig`.
