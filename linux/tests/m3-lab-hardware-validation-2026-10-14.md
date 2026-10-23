# M3-lab hardware validation (2026-10-14)

Board: M5Stamp ESP32-P4 + C6 (SDIO), P4 USB Serial/JTAG on COM10.  Only the
P4 flash was written; the C6 was not touched.

## Recovery point

Full 16 MB read-back before any write, from the image shipped on the board
(v0.2.1 era):

```
backup-2026-10-14.bin  sha256 31b136edcac1c4f9773d455571e01397da8a296c65165a4249c324b54f5afa5b
restore: python -m esptool --chip esp32p4 --port COM10 -b 921600 write_flash 0x0 backup-2026-10-14.bin
```

The restore was exercised once during the bisect below and booted with wlan0.

## Final candidate ("D")

Built by `build-c68.sh --profile m3-lab` from a staged copy of the tree, so
`/etc/build-info` reports `git=51decbdca0c1 dirty=1`
(`version=v0.2.1-smp-25-g51decbd`, kernel 6.18.35).  The staged tree matches
commit 20bebd5 for every file that reaches the image.

```
298a618c17d2b63320520d53a5205502ec0c86c37ef6ae0ebfa6b98ebeae3b32  Image
3540fdd5a350953148063244dd0583d186e2a256928130271530c9ae05a5d55c  boot-shim.bin
47a395e21f0dd6eb3990387fcbf0baa0d06a80c415be94a96fbcdf6b281c50ed  boot.img
9581932b9bd542b84e06d43ba7eef997315c3a1cc43abb5c00c9516e6f8ded10  bootloader.bin
cda3cbe1f944e5c6f5dd25dcca2017158cb3248542f8068c8a9bdfbfd9944989  esp32p4-m5stamp-p4.dtb
7937656f29960d53b63010b51836e233d092e067e320cde5778ec43e04218870  partition-table.bin
b462a4be7a5c37ce98ae554d1ea97a0730bb0372884f875a5fea25acb276fe79  rootfs.squashfs
```

Flashed with `flash-candidate.ps1 -AllowCandidateWrite` (write + verify passed).

## check-boot-log.py results

| capture | image | result |
|---|---|---|
| boot-orig | image shipped on the board | 9/11: `ES_HOSTKEY GEN_FAIL`, `start-stop-daemon: applet not found` (known, fixed in tree) |
| boot-A | tree without patch 0034, PIO default | 9/11: wlan0 never appears |
| boot-B | DMA default | 11/11 |
| reboot-B | B, `reboot` from the console (`--after-reboot`) | 11/11, no crash capsule |
| boot-C | B + `0<>` fix | 11/11 |
| boot-D | C + modprobe options fix | 11/11 |

## Regressions found and fixed on hardware

1. **wlan0 missing (PIO SDIO default).**  The C6 boot event arrived garbled
   (`Drop invalid pkt len=3 offset=0`), so no wlan0.  Not 0034 and not BT
   (boot-A was built without 0034 and still failed).  The shipped v0.2.1
   used the IDMAC path.  Fixed in 6c341f5: m3-lab defaults to
   `SDIO_FORCE_PIO=0`, `IDMAC_NONCOHERENT_RING=1`.
2. **/boot mount point missing.**  The squashfs root is read-only, so S05
   could not create it.  Fixed in 78f1c86 (`ES_BOOT_FAT OK`).
3. **Console keystrokes stolen.**  hush applies a bare `<>` to fd 1, so
   `read -t N _ <> fifo` left fd 0 on /dev/console (seen in `/proc/<pid>/fd`).
   Fixed in ca06cf7 (`0<>`, plus a static-check guard).  The console
   takes commands normally afterwards.
4. **USB LUN had no backing file.**  busybox-minimal drops modprobe
   command-line parameters, giving `no file given for LUN0` and bootsync
   `ownership=error`.  The shipped image only worked because of a stale
   `/etc/modprobe.d/g_mass_storage.conf`.  Fixed in 20bebd5.  On D:
   `parameters/file=/tmp/boot.img`, `[bootsync] ownership=host-owned`.

## Smoke-test results (smoke-test.md)

- Step 1–2: boot and reboot pass (table above).
- Step 3: `netstat` shows :22 LISTEN, udp 67 and 5353; `sysinfo` runs;
  MemAvailable 6.3–6.9 MB.
- Step 5 (#17): **not run.**  `CONFIG_SHMEM` depends on MMU, so `tmpfs` is
  ramfs: `size=4M` is ignored and `df /tmp` reports 0.  An 8 MB fill would
  OOM the board.  The dead config lines and the step were corrected in
  9b7a6cf.  #17 item 1 remains open (a brd+ext2 /tmp is one option).
- Steps 4, 6 (24 h soak), 7: not run.  Step 8 (pstore): see below.

## Issue re-checks on hardware (candidate D unless noted)

- **#15**: load average 0.00, no task in D state.  Over 60 s idle the
  busy delta in `/proc/stat` was 1 tick of 6000 (about 0.02 %; v0.2.1 7.2 %).
- **#19**: `reboot` still resets through the MWDT (`rst:0x7`) but writes no
  CRASH_CAPSULE (`check-boot-log.py --after-reboot` PASS on B and G).  The
  0059 injector is not in the lab image (`/sys/module/esp32p4_wdt` absent).
- **#5**: DTB `maxcpus=1` patched to 2 on D (one byte at 0xf10474).  Output
  stops after `ES_DWC2_STATUS` as reported, then the board stayed silent
  for more than 11 minutes with **no watchdog reset** and the ACM console
  did not answer.  Recovered by re-flashing the original DTB.  Not fixed.
  Repeated on H with `maxcpus=2 loglevel=8` (same DTB length):
  `smp: Brought up 1 node, 2 CPUs`, userland runs up to `ES_SEEDRNG OK`,
  then the console prints `Guru Meditation Error: Core 1 pan` (the ESP-IDF
  panic handler of the boot-shim, not Linux) and the MWDT resets the chip
  mid-line.  The pstore console record of the hung boot ends without any
  Linux oops.  (The ROM line `Core1 Saved PC:0x4ff0160a`, the GO-poll loop
  in `c68_secondary_entry.S`, also appears after a normal UP boot, so it is
  not evidence about the fault.)  The earlier run hung with no reset at all.
  Things ruled out:
  - The `ST_CONF`/`ST_INT_ENA` lock of 0045, which made the 2026-08-19 SMP
    profile pass, is already folded into the current 0042 (`st_lock`).
    0044/0045 no longer apply on top of it.
  - The Type-A USB gadget.  The host enumerates it right before the stop
    (`new device is full-speed`), but candidate J (H with the gadget
    skipped when the command line has `es.nousb`; a staged test change
    that was not committed) still fails with `maxcpus=2 es.nousb=1`: the
    first boot gets past S40 (`ES_SDIO_LOAD_OK`) and then prints the same
    `Guru Meditation Error: Core 1` before an MWDT reset.  The second boot
    reaches `udhcpd started` and then reports
    `rcu_sched kthread starved for 5250 jiffies! ... ->state=0x0 ->cpu=0`:
    the RCU kthread is runnable on CPU0 but never runs.
  The fault follows interrupt/DMA load (USB, then SDIO/Wi-Fi), not a fixed
  init step.  The SMP profile that passed in August had neither USB gadget
  nor Wi-Fi.  A lost cross-CPU wakeup (IPI) is the leading suspect; it is
  not proven.  Root cause not found.

  Follow-up later the same day:
  - 0066 (shared CLIC IRQs routed to CPU0 under SMP, bcbf2a2) and 0067
    (the ROM cache-sync engine serialised across CPUs, f7e4e46).  With both,
    candidate Q booted SMP to login and ran 200 `dropbearkey` runs (about
    8,800 IPIs) for about 20 minutes with no fault.
  - **The `Guru Meditation Error: Core 1` line is not the fault.**  The
    text comes from the ESP32-P4 ROM exception handler, not from the
    shim's panic handler: a wrapped `esp_panic_handler` never ran, and the
    IDF USB-Serial-JTAG writer would not explain the constant truncation
    either.  A counter in LP RAM showed CPU1 entering
    `c68_secondary_entry` twice per SMP reboot.  The MWDT system reset
    keeps the APP CPU boot address.  The ROM restarts CPU1 in the stale
    entry, which finds `c68_release` still GO and jumps to uninitialised
    PSRAM, and the ROM prints the Guru while the chip resets.  Every SMP
    `reboot` printed it; UP reboots never did.  The earlier "Guru" hangs
    were therefore watchdog resets after a real hang, with this line as a
    by-product.
  - Fix: the shim clears the boot address (`ets_set_appcpu_boot_addr(0)`)
    once CPU1 reports WAITING.  On candidate X (fix plus counter): one
    entry per boot and 0 Guru in 3 SMP reboots.  On candidate Y (the fix
    alone): UP cold boot 11/11, UP reboot 12/12, an SMP cold boot plus 3
    SMP reboots all reached login with 2 CPUs, no Guru and no stall, and
    11/11 again after restoring the `maxcpus=1` DTB.
  - **Still open:** an intermittent RCU stall early in SMP boots, seen on
    1 of 3 reboots on candidate X:
    `rcu_sched kthread starved for 5246 jiffies! ... RCU_GP_WAIT_FQS(5) ->state=0x0 ->cpu=0`,
    `All QSes seen`.  It appears right after the init scripts start
    `udhcpd`, and again at 21001 jiffies.  The GP kthread is runnable on
    CPU0, but CPU0 does not schedule it.  The board recovers and reaches
    login.  The stall dump printed no stack.  This is the remaining #5
    symptom, and its root cause is not known.
  - The stall was then hunted on candidate Y, using SMP reboots with
    `maxcpus=2`.  **In every stall the late CPU is CPU1, and CPU1 has
    stopped taking interrupts while it keeps executing**:
    - `1-...!: (0 ticks this GP) ... softirq=570/570`: no tick at all.
    - `rcu_sched kthread timer wakeup didn't happen for 5251 jiffies! ... Possible timer handling issue on cpu=1`:
      a timer queued on CPU1 never fired.
    - At 21007 jiffies the GP kthread is in `->state=0x200`
      (TASK_WAKING).  It is queued for CPU1, which never processes the
      wake-up IPI.
    - The RCU watching counter of CPU1 still changes between reports
      (`97b4` → `6b8c`), so CPU1 is not halted.  It looks like an idle
      loop whose `wfi` returns but never takes the pending interrupt.
    - Another run: `1-...!: (2 GPs behind) ... softirq=630/630 fqs=0`,
      then an MWDT reset.  A third run went fully silent with no MWDT
      reset for more than 2 minutes, which is the original #5 report.
  - **Root cause, at register level (2026-10-15, opt-in patch 0068,
    `C68_DEAF_TRACE=1`, candidates Z2–Z4):** CPU1's CLIC stops
    delivering interrupts that are pending, enabled and above threshold.
    This is the CLIC interrupt-acceptance latch documented in the vendored
    why2025-linux tree (`docs/KNOWN-ISSUES.md`, `docs/RUNTIME-WEDGE.md`),
    there seen on a single core and with no software recovery.  The
    ES_DEAF0 report caught four stalls in about 21 SMP boots (Z2, Z3,
    Z4, and one `nohlt` boot), and all four show the same state:
    - Routing is correct: INTMTX1 sends source 54 (CPU1 timer) to slot 23
      and source 80 (IPI) to slot 20, and INTMTX0 does not route either.
      SYSTIMER target 1 is raised (`st_raw=st_st=3`).  The IPI register
      stays at 1, and 200 ms after an extra IPI CPU1 has still not ticked.
    - CPU1's own record (Z4): CLIC slots 20 and 23 are both `ffc00101`
      (IP=1, IE=1, level 0xFF), the CLIC threshold is 0, and
      `mstatus=00001880`.  In just over a second CPU1 entered
      `arch_cpu_idle` 956397 times: `wfi` returns at once because an
      interrupt is pending, the idle loop re-enables MIE, and the
      interrupt is still not taken.
    - On the deaf CPU1, reading the CLIC CSRs `mintstatus` (0xfb1, Z3)
      or `mintthresh` (0x347, Z4) stops the core for good.  The
      assist_debug PC recorder then holds that one `csrr` (Z3
      `arch_cpu_idle+0x60`, Z4 `+0x146`) with SP unchanged.  The record
      reads them last for that reason.  (Corrected 2026-10-19: the read
      faults as an illegal instruction.  On one later boot pstore held the
      panic that followed; none of it reached the serial log.  See
      `issue5-smp-2026-10-19.md`.)
    - It does not depend on `wfi`.  With `nohlt` (polling idle) one of
      8 boots stalled the same way, with CPU1 in `rwsem_spin_on_owner`.
      Its PC still moved, but it took no tick and no IPI.
    - The last trap CPU1 took before going deaf was a system call from
      userspace: `mcause=3800000b` (ecall, `mpil=0`) in Z4, and
      `LASTPC_BEFORE_EXCEPTION` in userspace (`0x49c24026`) in the
      `nohlt` stall.  why2025 avoids its trigger (an interrupt racing a
      privilege-dropping `mret`) by running userspace in M-mode, and
      this kernel does too (`mstatus.MPP=M` above).  So on CPU1 the latch
      has another trigger, apparently near the syscall entry/exit.  That
      is inferred from two captures, not proven.
    - The stall messages on CPU0 follow from this: the GP kthread is
      queued on or waits for CPU1, and CPU0 can spin on a lock CPU1
      holds (PREEMPT_NONE).  Sometimes the board resets via the MWDT, and
      sometimes it hangs silently.
  - **Workaround verified on hardware: keep userspace off CPU1.**
    Candidate F (main, no 0068) with `maxcpus=2 isolcpus=1` (written in
    place of `loglevel=4`): both CPUs online (`online=0-1`,
    `isolated=1`).  17 boots: 0 RCU stalls and no MWDT reset other than
    the reboots themselves.  Without isolation, stalls hit 4 of about 21
    boots (about 19%), so 0 of 17 by chance has p ≈ 0.03.  One further
    boot was soaked for 568 s and stayed responsive.  This supports the
    syscall-side trigger above.  It does not identify the exact cycle
    window.
  - Consequence: the shipping default stays `maxcpus=1`.  With
    `isolcpus=1`, CPU1 runs only per-CPU kernel threads.  0066 already
    routes the shared IRQs to CPU0.  Turning SMP on that way gains almost
    nothing.  It would also leave a latch that any `taskset` onto CPU1
    could set off again, and it has not been tested under Wi-Fi load.
    It remains a documented option for SMP experiments.  Recovering is
    not an option: the deaf core cannot even read its own CLIC state,
    and why2025 found no recovery.
  - (2026-10-19: the trigger was found and fixed by 0079, which also
    fixes a 1-CPU hang; both CPUs are now on by default.  See
    `issue5-smp-2026-10-19.md`.)
  - `Core1 Saved PC` in the ROM banner is the assist_debug PC recorder,
    frozen at reset.  Without 0068, nothing enables it for core 1, so it
    shows the stale `0x4fc058e0` (the eco2 ROM getter for the APP CPU
    boot address, `lw 0x50110164`) after every reset.  With 0068 it
    shows CPU1's real PC at the reset (e.g. `arch_cpu_idle+0x146`).
    A shell watcher that read `/proc/interrupts` through `$(...)`
    OOM-killed the console (the known NOMMU limit).
- **#26 pstore** (candidate G = D + `PSTORE=1`
  `SDIO_MAX_KHZ=5000`; candidate E, the same without 0065,
  panicked at boot with `kernel BUG at mm/nommu.c:311`):
  - `echo c > /proc/sysrq-trigger`: reset via MWDT, then
    `ES_PSTORE OK: 1 record(s)`; `console-ramoops-0` holds the panic and
    call trace.
  - `panic=0` + sysrq: the kernel hangs, the MWDT resets the board within the
    7-minute capture window, and the record survives.  PSRAM keeps ramoops across a
    watchdog reset.
  - G had no `dmesg-ramoops-*` record and the console record stopped at the
    call trace: PSRAM is behind a write-back L1D/L2 that the reset drops.
    Candidate H (G + zone writeback in 0065) after clearing
    `/sys/fs/pstore`: sysrq panic gives `dmesg-ramoops-0` (11672 B) and a
    console record ending in `Rebooting in 5 seconds..`; the panic=0 hang
    reset by the MWDT gives a fresh `dmesg-ramoops-0` (11832 B) containing
    `sysrq triggered crash`.
- **#21-1**: with the cap at 5 MHz, `ES_SDIO_CLOCK_HW ... actual_clock_hz=5000000`;
  cold boot 11/11 and reboot 12/12 PASS.  Throughput (`scp`) and the soak
  were not run (no Wi-Fi client attached), so the DTS default stays 400 kHz.
- **#17 item 1 (`/tmp` cap, 895f450)** on candidate Y: `/dev/ram0 on /tmp
  type ext2`, 1979 KB.  `dd` of 8 MB into `/tmp` stops with
  `No space left on device` after 422 blocks (about 1.7 MB).  No OOM
  appeared in dmesg, the SSH listener stayed up, and MemAvailable fell
  from 6756 KB to 5452 KB.  Removing the file restored the original
  283 KB used.  This ran as root, because the image has no `su`; the cap
  does not depend on the user.
- **First-boot OOM (#17, #29), 9210dc2**: three resident respawn
  subshells (about 500 KB of BusyBox each on NOMMU) made S40's `conf_get`
  pipelines fail on the first boot and print an empty SSID.  The new
  `respawn` supervisor (about 110 KB) replaces them.  On
  candidate P, the first boot after flashing gave 11/11 with the right
  SSID and no OOM, and a reboot gave 12/12.  Still seen: one `sed`
  OOM-killed during host-key generation on a reboot.  Nested `$(...)` on
  the console can still exhaust memory.
- Build: an incremental build kept the previous kernel when 0065 was added
  (b89fcbb now dircleans linux when the staged series changes).

## Follow-up on 2026-10-15 (candidate AK: e3e9cb0 plus 43fd110)

- **#29 item 3, kernel built with `-Os` (43fd110)**: vmlinux text went
  from 5520427 to 4630157 bytes and the Image from 6.5 MB to 5.6 MB.
  `Memory: 19844K/32768K available (3034K kernel code, ...)`, and
  MemTotal went from 20176 kB to 21040 kB.  The flash boot gave 11/11,
  and ten more resets (`capture-boot.ps1 -Reset`) each gave 11/11.
  `iw dev wlan0 scan` listed 13 BSS over SDIO.  Throughput was not
  measured (no Wi-Fi client).
- **nommu-pool (#29)**: the 6 MB `linux,nommu-userspace-pool` at
  0x49a00000 comes from the vendor patch `0010-mm-nommu-userspace-pool`.
  User mmaps take the pool first, then buddy pages through
  `alloc_pages_exact`.  The pool ends at the top of PSRAM and cannot grow
  down without moving the boot FAT reserve at 0x499c0000, which the shim
  uses at a fixed address.  No reason for the 6 MB size was found beyond
  the vendor description, so it was left alone.
- **#26 `debug=`**: the cmdline boots with `loglevel=4`, and
  `/proc/sys/kernel/printk` is `4 4 1 7`.  With `debug=1` added to
  `/tmp/config.txt` and S40network restarted, S40 printed
  `M3-lab: debug=1: console loglevel 8`, printk became `8 4 1 7`, and a
  `<7>` line written to `/dev/kmsg` reached the console.  After restoring
  the file and restarting again, printk was back to `4 4 1 7` and a `<7>`
  line stayed off the console.  `ES_WDT` (KERN_INFO) is only in dmesg
  unless `debug=1`; `OBS` markers are in opt-in builds only.
- **#26 pstore on by default (candidate AL: 6fe7bbe, built without
  `PSTORE`)**: the boot reserves `ramoops@499af000` (64 KiB)
  and logs `pstore: Registered ramoops as persistent store backend`;
  S05 printed `ES_PSTORE OK: 0 record(s) in /sys/fs/pstore` and the
  check gave 11/11.  `echo c > /proc/sysrq-trigger` panicked the kernel,
  the board rebooted on its own (`panic=5`), and `/sys/fs/pstore` then
  held `dmesg-ramoops-0` and `console-ramoops-0`, both with
  `Kernel panic - not syncing: sysrq triggered crash`.

Flashing E/G rewrote the boot partition from the template, so the board
generated new credentials (SSID `m5-F314`, then `m5-4D54` after H and
`m5-73D7` after J; see credentials.txt on the USB drive).

## Board state at the end of the session

Running candidate F (main at 2ed8648 built without
`C68_DEAF_TRACE`, so the same kernel as candidate Y) with the
normal `maxcpus=1` DTB, 11/11 PASS; SoftAP at 192.168.4.1
(credentials in credentials.txt on the USB drive).
The backup
above restores the original image.
