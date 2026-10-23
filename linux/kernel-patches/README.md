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
Stamp-P4-specific deltas here. The Stamp-P4 DTS in `../dts/` is the only
board description allowed; the WHY2025 DTS and its SPI C6 nodes must not be
applied.

`0022-esp32p4-systimer-level-trigger.patch` is an Stamp-P4-owned extraction
of the driver hunk from the reference `0022` patch. The reference file also
edits the WHY2025 DTS, which is intentionally not applied; the board-owned
DTS in `../dts/` already selects `IRQ_TYPE_LEVEL_HIGH`.

`0035-esp32p4-wdt-feed-observe.patch` is m3-lab-only observe
telemetry on `esp32p4_wdt_ping`: it prints `ES_WDT WDT_FEED_COUNT` and
`last_feed_jiffies` without changing timeout, stage policy, or feed rate. It prints the first 16 feeds and then only
power-of-two counts, so a long uptime does not fill the kernel log (#12).

`0036-tcp22-bidirectional-ledger.patch` is m3-lab-only observe
telemetry for TCP port 22 (`ES_TCP` markers on sendmsg/push/write_xmit,
ip_queue_xmit, dev_queue_xmit, ip_rcv, tcp_v4_rcv, established, data_queue,
and ACK build/xmit). It does not change TCP behaviour, Nagle, or sysctls.

`0037-c67-l2-stage-smpboot.patch` is enabled for C67-L2-STAGE
(`C67_SMP_STAGE=1`) and the C68 fail-closed profile. It adds
observe-only `pr_info` breadcrumbs in `arch/riscv/kernel/smpboot.c`:
`C67 UP`, `C67 START`, `C67 CALLIN`, `C67 ONLINE`, then after
`complete(&cpu_running)` `C67 COMPLETE`, immediately before
`cpu_startup_entry()` `C67 STARTUP`, and on CPU0 `__cpu_up()` success
return `C67 UP_RETURN`. It does not change CPU start policy, timeouts,
or IPI handling.

`0038-c68-clean-release-spinwait.patch` is a **template**. It
keeps `0xC68DEAD1/2` placeholders in Git. `linux/build-c68.sh` renders it
into `<output>/c68-rendered-patches` after extracting boot-shim `nm`
addresses. `build-m1.sh` with `C68_CLEAN_RELEASE=1` refuses the
kernel build unless that override directory is set, is not the patch
stage, and contains no `C68DEAD` bytes.

`0039-c68-clean-release-smpboot.patch` is rendered the same way.
It extends the C67 observe breadcrumbs with C68 timeout diagnostics in
`__cpu_up()` without duplicating the C67 marker set.

`0040-esp32p4-ipi-provider.patch` extends the ESP32-P4 CLIC
interrupt-matrix mapping to both HP harts and adds the `CPUHP` anchor used by
the provider. `0041-esp32p4-ipi-driver.patch` maps the
`CPU_INT_FROM_CPU_0/1` sources, multiplexes them into the generic RISC-V IPI
range, and calls `riscv_ipi_set_virq_range()` only after the CPU-starting hook
has been installed. The C68 fail-closed gate checks that both patches, the
provider symbol, and the IPI DTS node are present in the same staged triple.
The 2026-08-19 A' capture passed that virq-range gate in hardware:
`smp.c:176` was absent and `Brought up 1 node, 2 CPUs` was observed.

`0042-esp32p4-systimer-cpu1.patch` gives SYSTIMER target 1 and its
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

`0045-esp32p4-systimer-st-conf-lock.patch` serializes the shared
`ST_CONF`/`ST_INT_ENA` programming sequence with a raw spinlock while
preserving the target-local write-one `COMP*_LOAD` operation. Its standalone
120 s hardware control still reached the CPU1 illegal-execution/RCU/CRED
failure, so it is not accepted as a complete fix by itself.

`0046-esp32p4-cpu1-context-observe.patch` is an observation-only
patch. It reports CPU1 illegal traps and sampled CPU1 context switches; it
does not change the hotplug, IPI, or timer policy. The first coherent
0045+0046 hardware shot reached `Run /sbin/init`, observed valid CPU1 context
switches, CPU1 timer IRQ delivery, and bidirectional IPIs without an illegal
trap, RCU stall, or panic. This is **L2.5 PASS for the observation triple**,
but not an L3 result and not proof that the observer fixed the race: its
`pr_info()` in `context_switch()` changes timing. The next control must retain
0045 while removing that printk.

`0047-esp32p4-cpu1-illegal-observe.patch` is that low-overhead
control: it retains only the CPU1 illegal-trap snapshot from 0046 and contains
no scheduler/context-switch printk. With 0045 and 0047 selected, the ordinary
path has no new log operation; the observer runs only if the illegal trap
recurs. Its staged hash and complete triple must be recorded before any
hardware classification.

`0048-esp32p4-l3-smoke-endpoint.patch` is selected only for the
explicit C68 L3 smoke profile. It creates the test-only
`/proc/m5stamp_smp_smoke` endpoint. A userspace write runs a bounded,
CPU-verified CPU0 -> CPU1 `smp_call_function_single()` and a non-blocking
CPU1 -> CPU0 reverse notification, then reports the callback CPU identities
and return codes. The reverse leg is intentionally non-blocking while the
CPU0 callback returns; waiting inside that callback would deadlock the CPU0
IPI handler. It does not alter the generic IPI provider, timer policy, or
scheduler path, and it is not included in ordinary C68 or M3 images.

`0049-esp32p4-l3-smoke-workqueue.patch` completes that reverse leg
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
`TSENS_LINUX=1` on the C68 build path, and is intentionally separate
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

`0077-esp32p4-mret-trace.patch` (`C68_MRET_TRACE=1`,
C68 profile only) is an observe-only #5 diagnostic: entry.S records a
per-CPU ring of trap entries (raw mcause incl. hardware MPIL, mepc,
mstatus; the mintstatus slot is always `0xffffffff`, since `csrr` of
`0xfb1` at trap entry was an illegal instruction at boot), a ring of
mret writes (the mcause the exit shim restores, mepc, PT_TP vs tp,
status), an interrupt-only ring that freezes when the hart goes deaf
(snapshots the all-traps and mret seqs), and an 8-entry post-interrupt
mret buffer re-armed by each accepted interrupt.  Nonzero-MPIL mret
writes are counted in `es_mret_bad_mpil`, and a CPU0 timer-tick watch
dumps the rings (CPU1 in full, CPU0's newest 8) as
ES5_WATCH/ES5_TRAP/ES5_IRQ/ES5_MRET/ES5_POST lines when CPU1's stamp is
2 s stale.

`0078-esp32p4-deaf-reset.patch` (`C68_DEAF_RESET=1`,
C68 profile only) is the matching #5 mitigation: a per-CPU jiffies stamp
written by each CPU's own SYSTIMER interrupt lets CPU0 notice a deaf
CPU1 after 3 s and take the chip down through `emergency_restart()`
(`esp32p4_wdt_restart`, priority 128) with a panic() backstop, so a
latched boot becomes one bounded retry.  Both default OFF.

Detection order for #5 is deliberate: 0068 ES_DEAF at 1 s + 200 ms, then
0077 ES5_WATCH at 2 s, then 0078 ES5_RESET at 3 s.

`0079-esp32p4-irq-level-drop.patch` is the #5 fix and is applied
to every build (`C68_IRQ_LEVEL0=0` leaves it out for A/B tests).
As far as the #5 captures show (it is not confirmed against Espressif
documentation), this CLIC raises `mintstatus.mil` to the level of an
accepted interrupt and lowers it only on an mret with `mcause.INT=1`.  An interrupt taken from
user mode that ends in `schedule()` can leave the hart at level 0xff, deaf
to every interrupt, until the preempted task returns through its own frame
on that hart; if the task migrates, is stopped or dies first, the hart stays
deaf.  0079 runs one
mret to the next instruction right after the frame is saved (INT=1, MPP=M,
MPIE=0, MPIL=0), so `mil` is 0 before `do_irq` and an abandoned frame holds
no level.  The 1-CPU image needs it as well: without it, killing a busy
task wedged CPU0 until the MWDT reset the chip.

`0080-esp32-acm-tx-irq-lock.patch` takes `port->lock` around the
transmit in the USB Serial/JTAG ACM interrupt handler.  Without it the
handler and `start_tx` on the other CPU drained the `xmit` kfifo at the
same time, `out` passed `in`, and ttyGS1 re-sent its 4 KiB buffer in a loop.

`0082-esp32-acm-tx-irq-console.patch` (#42) fixes two faults of
the same port under load.  When the host had not read the previous packet
yet, `transmit_buffer()` returned without arming the IN-empty interrupt, so
the end of the output (the last line's `\r\n`) waited 0.5 s for the serial
core's runtime PM to call `start_tx()` again; it now arms the interrupt
whenever it leaves data behind.  Console output committed each character as
a packet of its own and dropped a character after waiting 2 ms for the host
(0062); it now commits once per write and waits up to 50 ms counted from the
last byte the FIFO took, so a host that stops reading costs one wait, not one
per character.  Characters dropped after that are counted in
`/sys/class/tty/ttyGS1/console_dropped`.

`0081-esp32p4-expt-clear.patch` is applied to every build as well
(`C68_EXPT_CLEAR=0` leaves it out).  The core sets
MEXSTATUS.EXPT_VLD (CSR 0x7e1, bit 5) when it takes an exception, and only
an mret with `mcause.INT=0` clears it.  An exception other than ecall taken
while the bit is set is a double exception: the core locks up at the
faulting instruction until the MWDT resets the chip.  A system call that
ends in `schedule()` and resumes a task from its interrupt frame (INT=1)
left the bit set in that task, so the task's next fault (a SIGSEGV, handled
or not) stopped the core, on one CPU as on two.  0081 is the exception-side
counterpart of 0079: one mret to the next instruction once the frame is
saved (INT=0, MPP=M, MPIE=0), so the bit is clear before the handler runs.

`0083-esp32p4-sigreturn-in-kernel.patch` (#44, every build) makes
a signal handler return through the kernel's own `__user_rt_sigreturn`
(`li a7, __NR_rt_sigreturn; ecall` in `.rodata`).  Without a vDSO the NOMMU
code copies those two instructions to the signal frame and calls
`flush_icache_range()`, which is `flush_icache_all()`: with two CPUs online
an IPI to the other one and a wait for it, which made a handled signal on two
CPUs almost twice as slow as on one.  User code runs in M-mode here (0031)
from the same PSRAM window as the kernel image, so it can run the kernel's
copy, and neither the copy nor the flush is needed.  An unwinder that finds
a signal frame by those two instructions at `ra`, as libgcc's does, sees the
same bytes.  20,000 returns from a SIGSEGV handler (40,000 faults) take 0.7 s
on either CPU, against 2.5 to 2.7 s before and 1.49 s on one CPU without
0083.

`0084-mmc-sdio-irq-highpri.patch` (#43, every build) queues
`host->sdio_irq_work` on `system_highpri_wq` instead of the normal workqueue
when `sdio_signal_irq()` runs.  The work is the first hop of every packet
from the C6, and under CPU load it sat a tick or more behind normal-priority
work while everything after it was mostly under a millisecond once it ran:
the debug build measured the whole signal -> handler -> read path under 1 ms
in more than 99 % of cases, so the queue was the delay, not the work.  The
esp-hosted package patches `0039` and `0040` cover the rest of the same
path: waking `esp_TX` when a packet is queued, and running the RX workqueue
and the TX thread ahead of nice 0.

`0072-esp32p4-bootflash.patch` (`/sys/kernel/board/boot_commit`,
every build) writes the `/boot` drive to flash with the ROM's SPI1 routines.
Each ROM read, erase and program runs under `stop_machine()`: CPU0 makes the
call with interrupts off while the other CPU spins with interrupts off, so
nothing reads the rootfs from flash meanwhile.  Before #5 it returned
`-EBUSY` with two CPUs online, and every USB drive save rebooted the board so
that the boot-shim would write the slot.

Nothing else runs during a ROM call, not even the SDIO interrupts that carry
Wi-Fi.  A commit used to make about 1,350 calls back to back (both slots
read to find the active one, the active slot read again, all 64 sectors of
the inactive slot erased, programmed and read back), which held the CPUs for
nearly all of its 1.9 s; pings waited up to 0.9 s and some were lost (#43).
It now skips the inactive-slot sectors that already hold the image (the slot
usually holds the generation before the active one, and an edit changes a
few sectors), reads the older record's slot only when the newer record is
not valid, and after every 8 ms of ROM calls (`FLASH_BURST_US`) sleeps for as
long as they took.  The kernel log line `boot update committed: … sectors=N/64
rom=…ms total=…ms` gives the sectors written and the time in ROM calls.  A
commit that writes 0 to 2 sectors takes 0.6 to 0.75 s (about 0.2 s of it in
ROM calls, most of them reads); one that rewrites 63 sectors 3.5 to 4 s
(1.5 to 1.7 s in ROM calls).  With six commits in a row of either kind, 65
of 65 pings came back, the slowest in 280 ms; see
[`../tests/issue41-44-2026-10-20.md`](../tests/issue41-44-2026-10-20.md).

With 0079 and 0080, the C68 profile (`build-c68.sh`, used by the release
workflow) boots both CPUs by default; `C68_MAXCPUS=1` builds the
1-CPU image.  A 2-CPU build with `C68_IRQ_LEVEL0=0` brings the CPU1
latch back.  The boot-shim gives CPU1 CPU0's PMA/PMP setup before CPU1
waits for Linux; without it a user fault on CPU1 arrived late, inside the
trap handler, and panicked the kernel.  See
[`../tests/issue5-smp-2026-10-19.md`](../tests/issue5-smp-2026-10-19.md).

This directory documents a real dependency boundary while the series is
still under review. A clean build must fail early if the baseline symbols are
absent rather than silently dropping UART, watchdog, or NOMMU support during
`olddefconfig`.
