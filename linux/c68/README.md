# C68 fail-closed SMP profile

This directory is the C68-CLEAN-RELEASE profile.  `build-c68.sh` and the
release workflow build with it, and since #5 it boots both CPUs
(`maxcpus=2`); `EASYSTICK_C68_MAXCPUS=1` keeps CPU1 parked.

- Default `linux.config` stays `CONFIG_SMP=n`; this profile appends
  `linux.config.fragment`.
- Default `dts/easystick-stamp-p4.dts` stays `cpu@0` only; this profile
  inserts `cpu1.dts.inc` and `ipi.dts.inc`.
- Two CPUs rely on 0079 (`EASYSTICK_C68_IRQ_LEVEL0`, default 1), which
  stops a CPU from going deaf to interrupts, and on 0080, which locks the
  ttyGS1 transmit path.  Both apply to every build, as does 0081
  (`EASYSTICK_C68_EXPT_CLEAR`, default 1), which keeps a user fault from
  locking a core up.  `IRQ_LEVEL0=0` with two CPUs brings the CPU1 latch
  back; see `../tests/issue5-smp-2026-10-19.md`.
- The boot-shim copies CPU0's PMA and PMP CSRs to CPU1 at the start of
  `c68_secondary_entry`, reads them back and prints
  `C68 CPU1 PMA/PMP diff=00000000`; `check-boot-log.py` fails any other
  value, and a boot that parks CPU1 without the line.  Without the copy CPU1
  kept the ROM's open setup, and a user fault on CPU1 panicked the kernel.
- 0072 makes each flash read, erase and program under `stop_machine()`, so
  CPU1 cannot read the rootfs from flash while SPI1 writes it.  Before, it
  refused to write with CPU1 online, and every USB drive save rebooted the
  board.
- Enter with `linux/build-c68.sh` (boot-shim nm → render 0038/0039 →
  stage 0040/0041/0042 → `build-m1.sh`).
- Do not set `EASYSTICK_KERNEL_PATCH_OVERRIDE_DIR` to the kernel patch stage.
- 0037 on this profile includes COMPLETE / STARTUP / UP_RETURN observe
  markers. They belong only on an Image produced by `build-c68.sh`.
- 0040 extends the ESP32-P4 CLIC mapping to both HP-core interrupt-matrix
  banks.
- 0041 provides the generic RISC-V IPI mux and publishes
  `riscv_ipi_set_virq_range()` only after the CPU-starting hotplug callback is
  installed. The first A' gate is that CPU1 cannot enter
  `riscv_ipi_enable()` before this publication.
- `c68/ipi.dts.inc` adds the CPU_INT_FROM_CPU_0/1 routes (INTMTX sources 79/80)
  and is inserted only into the C68 staged DTS. The base DTS remains a
  single-core profile.
- 0042 gives SYSTIMER target 0/source 53 to CPU0 and target 1/source 54 to
  CPU1, with separate CLIC parent IRQs and CPU-starting clockevent setup.
  The A' timer gate is that CPU1 receives a real per-hart clockevent before
  entering its idle loop; a CPU0-only clockevent is not sufficient for RCU.
- Set `EASYSTICK_C68_L3_SMOKE=1` to include the renamed
  `/usr/sbin/m5stamp-smp-smoke` helper, the C68-only BusyBox `top`
  fragment, and the USB Serial/JTAG ACM-backed `ttyGS1` askfirst shell used
  by the COM10 capture. These additions are staged outside the repository
  and are never applied to the UP profile.
- Set `EASYSTICK_C68_STRESS=1` together with `EASYSTICK_C68_L3_SMOKE=1` to
  add `/usr/sbin/m5stamp-smp-stress`. The bounded helper exercises
  CPU1-affine work, fork/exec, allocation/free, repeated bidirectional IPI
  smoke calls, and non-interactive `top` snapshots. The standard capture
  invokes it for 90 s inside a 120 s UART window so the final PASS marker is
  observable; each `top` snapshot is capped at 4 KiB before it reaches the
  diagnostic UART. When the Linux TSENS option is enabled, it also samples
  the `esp32p4-tsens` thermal zone at approximately 1 Hz and reports only
  aggregate `temp_reads`, `temp_failures`, and min/max/start/end m°C values
  in the final PASS line. It is a diagnostic gate, not a release or
  manufacturing test.
- The non-network M1 post-build boundary removes `S01seedrng`: M1 has no
  writable `/var`, and the early read-only write attempt is outside this
  profile's scope and can expose a NOMMU startup fault.

See `../tests/c68-l2-hw-contract-2026-08-18.md`.
