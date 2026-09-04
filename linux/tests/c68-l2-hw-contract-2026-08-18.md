# C68-CLEAN-RELEASE contract: L2-v2 / L2-HW (2026-08-18)

Status: **L2-HW PASS 20/20 on a measured Image/DTB/boot-shim triple.
Functional SMP is not claimed. Source-to-artifact rebuild gate is open.**

This file versions the L2 contract after hardware evidence. It does **not**
rewrite an earlier L2 meaning in place. The original “generic SMP bring-up
complete” wording is **L2.5**, not L2.

## Recommended external wording

> **C68-CLEAN-RELEASE により、Linux が secondary hart を解放し、CPU1 が
> `smp_callin()` を経て online 状態へ到達する経路を RTS×20 で再現した。
> 機能的なSMPは、IPIおよびCPU1へのタイマ配送の実装・検証待ち。**

Do not describe C68 as “SMP達成” or “デュアルコア対応完了” by itself.

## Artifact hashes for the L2-HW evidence

From the 2026-08-18 RTS×20 run (`C:\Users\developer\tmp\easystick-m25-smp-20260817`):

| Artifact | SHA-256 |
|---|---|
| boot-shim | `d338a86ef08b1bb91267f4884aa835907fde1dd22f59df4a882ca4804360ab8b` |
| Image | `0f90d545724c9b8b5b85370ef92661859186a3ad1efd547d218bdfc976b426e6` |
| DTB | `a76df56172cc30a5d7f8b20d3dd8b24f66f6e67518ccf87a7d30d4240b8f639b` |

Extracted PAs recorded with that run:

```text
C68_RELEASE_PA=0x4ff13594u
C68_STAGE_PA=0x4ff13590u
C68_ENTRY_PA=0x4ff01620
```

Result: `L2_PASS=20 L2_FAIL_1CPU=0 C68_TIMEOUT=0 FAIL=0` with
`WAIT → RELEASE → CALLIN → ONLINE` on every valid attempt.

`HEAD` of this repository still has `CONFIG_SMP=n` and a `cpu@0`-only DTS.
The working tree currently carries uncommitted `CONFIG_SMP=y` /
`CONFIG_NR_CPUS=2` / `CONFIG_RISCV_BOOT_SPINWAIT=y` and `cpu@1`. Those
working-tree diffs are **not** the L2-HW closure: they are the gap C must
turn into a fail-closed C68 profile. The x20 evidence is valid for the
hashes above; it is not yet proof that `build-m1.sh` from committed sources
reproduces them.

## Milestone table (versioned)

| Level | Name | Pass condition | Verdict |
|---|---|---|---|
| **L2-v2 / L2-HW** | Secondary-hart release and Linux call-in | Canonical C68 artifacts show `WAIT → RELEASE → CALLIN → ONLINE`, `START ret=0`, `TIMEOUT=0`, no panic, RTS×20. Unimplemented IPI is an accepted L2 constraint. | **PASS 20/20 on hashed artifacts. Rebuild gate open.** |
| **L2.5** | Clean dual-CPU kernel bring-up | `riscv_ipi_have_virq_range()==true`, no `smp.c:176` WARN, generic SMP bring-up complete, CPU1 enters `cpu_startup_entry()`, init continues ≥120 s | **PASS on the isolated C68 image; source commit traceability remains** |
| **L3** | Operational SMP | `/proc/cpuinfo` and sysfs show 2 CPUs, CPU1-pinned work, bidirectional IPI, IPI counters increase, scheduler/RCU/timer smoke, no stall/panic | **PASS on three consecutive helper-pass captures of corrected `m1-topcapfix`; broader coverage remains** |

### L2-v2 / L2-HW boundary

```text
boot-shim WAITING
  → Linux publishes spinwait bootdata
  → RELEASE GO
  → CPU1 enters smp_callin()
  → set_cpu_online(1)
  ───────────────────── L2-v2
```

This guarantees secondary-hart Linux participation. It does **not** guarantee
IPI, CPU1 scheduler execution, per-CPU timer, or userspace load.

`CALLIN` and `ONLINE` are strong evidence that CPU1 executed Linux through
`set_cpu_online(1)`. After `ONLINE` the kernel still performs cache/TLB
flush, `complete(&cpu_running)`, local IRQ enable, and `cpu_startup_entry()`.
Current markers do not prove CPU1 reached scheduler idle or normal SMP
services.

### L2.5 boundary

```text
L2-v2
  → cache/TLB flush
  → complete(&cpu_running)
  → local IRQ enable
  → CPU1 startup/idle entry
  → CPU0 generic SMP loop returns
  → "Brought up 1 node, 2 CPUs"
  → init continues
  ───────────────────── L2.5
```

Linux 6.18.35 prints node count between the words. Detect with:

```regex
Brought up\s+\d+\s+nodes?,\s+2\s+CPUs\b
```

Do not require a exact match on `Brought up 2 CPUs`. L2.5 also requires
positive evidence that `riscv_ipi_have_virq_range()` is true. Absence of the
WARN alone is not a pass (a change that only silences the WARN must fail).

### L3

L3 is “Linux can actually use two CPUs”, not “two CPUs are listed”.

Minimum:

- `/sys/devices/system/cpu/online` is `0-1`
- `/proc/cpuinfo` has two processors
- CPU1-affine work completes on CPU1
- CPU0→CPU1 `smp_call_function_single()`-class call completes
- CPU1→CPU0 reverse notification completes
- `/proc/interrupts` reschedule / call-function / timer IPI counts increase
- scheduler, RCU, and timer show no stall under a bounded parallel load

`/proc/cpuinfo=2` alone is not L3.

## `smp.c:176` WARN

| Context | Treatment |
|---|---|
| L2-v2 / L2-HW | Allowed |
| L2.5 and later | Blocker |
| Any SMP-enabled C6/SSH integrated image | Blocker |

`riscv_ipi_enable()` WARNs and returns when `ipi_virq_base == 0`. CPU1 then
enables no per-CPU IPI IRQs (reschedule, call-function, CPU stop, IRQ work,
timer broadcast). The correct reading is:

> CPU1 reached the online bit; Linux cross-core services are not connected.

This is not an initialization-order bug to paper over by moving
`riscv_ipi_enable()`. The published ESP32-P4 CLIC driver does not call
`riscv_ipi_set_virq_range()`. The P4 SYSTIMER driver documents single-core
CPU0-only clockevents. The next SMP implementation step is therefore **A'**:

> Implement an ESP32-P4 cross-core IPI provider and make CPU1 timer delivery
> work via per-hart clockevent or broadcast IPI.

Hardware already has `HP_SYSTEM_CPU_INT_FROM_CPU_0/1_REG`. Linux should mux
one hardware notification into the generic IPI range and then call
`riscv_ipi_set_virq_range()`, similar to ACLINT SSWI.

## Next engineering change: C, then lanes

Order:

```text
Existing flash: one 120 s RTS capture (classification, not x20)  — done, outcome 3
  → C: close C68 source-to-artifact (profile, config, DTS, nm, placeholders)
      Entry point: `linux/build-c68.sh`. UP sources stay `CONFIG_SMP=n` / `cpu@0`.
  → Resume C6/SDIO/SSH on the UP profile
  → SMP lane: 0037 COMPLETE/STARTUP/UP_RETURN observed on the C triple
    (stop is in/after `cpu_startup_entry()`). Next is A'
  → L2.5
  → L3
```

Do not add those three observe points onto a working-tree or locally patched
Image. They belong only on an Image produced by `build-c68.sh`.

A–D: choose **C. build automation**. Placeholder `nm` substitution alone is
not enough. A C68-only fail-closed build must:

1. Build the C68 boot-shim first and extract `c68_release`, `c68_stage`, and
   `c68_secondary_entry` from the ELF.
2. Render 0038/0039 into a dedicated generated directory, physically separate
   from the patch stage.
3. When C68 is selected, require final kernel config
   `CONFIG_SMP=y`, `CONFIG_NR_CPUS=2`, `CONFIG_RISCV_BOOT_SPINWAIT=y`, and
   inspect the final `.config`.
4. Generate or canonicalize C68 DTS with `cpu@1` and its CPU
   interrupt-controller; decompile the finished DTB and inspect it.
5. Fail if staged patches, `vmlinux`, or Image still contain `C68DEAD1/2`.
6. Emit one manifest: source SHA, every patch SHA, extracted PAs, final
   config digest, DTB/Image/boot-shim SHA.

Default C6/SSH images stay `CONFIG_SMP=n`, CPU0-only. Do not mix unfinished
SMP into the SDIO timing/IRQ/DMA evidence lane. Issue #6’s first SSH
milestone does not require SMP.

C6 resume condition: C has canonized the current C68 artifacts and split UP
baseline from the SMP experimental profile.

- Parallel resume: after L2-HW **and** C
- Merge SMP into the default image: after L3

## 120 s classification capture (one shot)

Not a reproducibility trial. Do not reflash. RTS reset once. 120 s. No byte
cap. Search:

```text
C68 WAIT
C68 RELEASE
C67 CALLIN
C67 ONLINE
WARNING: CPU: 1 ... smp.c:176
Brought up <n> node(s), 2 CPUs
existing init markers
panic / watchdog / stall
```

Outcomes:

1. `Brought up 1 node, 2 CPUs` and init continues: CPU bring-up itself
   finished; remaining work is reproducibility and IPI/platform services.
   L2.5 still fails while the IPI WARN is present.
2. `Brought up ... 2 CPUs` but init does not proceed: generic SMP loop
   finished; stop is afterward (IPI, timer broadcast, later init).
3. No `Brought up ...` in 120 s: stop is between `ONLINE` and
   `complete(&cpu_running)` / CPU0 `__cpu_up` return. Next observe points
   only: immediately after `complete`, immediately before
   `cpu_startup_entry`, immediately before CPU0 `__cpu_up` return.

## 120 s classification (2026-08-18T13:41:52Z)

**Outcome 3, with the stop stated as an observation not a CPU halt.**
`ONLINE` 後の進捗が120秒間観測されなかった。That is not a claim that CPU1
stopped. USB-JTAG-only silence remains a secondary hypothesis; it is
weakened by `keep_bootcon` plus a byte-identical 4499 B stop versus the
50 s x20 windows.

No flash write. Reset was `esptool … default_reset / hard_reset chip_id`, the
same strap path as the x20 run. `capture-boot.ps1 -Reset` on this port
returned **0 bytes / 30 opens** and is not this classification (USB-JTAG
went deaf when DTR/RTS were forced low). The x20 serial constructor
(`serial.Serial(COM10, 115200)`) produced the log below.

| Field | Value |
|---|---|
| File | `C:\Users\developer\tmp\easystick-m25-smp-20260817\c68-120s-l25-classify.bin` |
| Bytes | 4499 |
| SHA-256 | `bc18c0772d87813b9dbc717d0016c19fc85b389a4642e6343f6f38593eeaa9f1` |
| First byte | 0.793 s |
| Port opens | 1 |
| Window | 120 s |

Hits: `C68 WAIT`, `C68 RELEASE`, `C67 CALLIN`, `C67 ONLINE`, `smp.c:176`
WARN, `C67 START cpu=1 ret=0`. Misses: `Brought up`, init markers, panic,
watchdog, soft lockup, RCU stall. Last printk:

```text
C67 CALLIN cpu=1 hart=1
C67 ONLINE cpu=1
C67 START cpu=1 ret=0
```

Byte length matches the x20 50 s windows that stopped one second after
`ONLINE`. Holding the same USB-JTAG handle for 120 s added **no further
bytes**.

**Classification: outcome 3.** No `Brought up … 2 CPUs` in 120 s. The stop
is after `ONLINE` / `__cpu_up` `ret=0` and before a console-visible
generic SMP completion. Next observe patches, if still needed after C, are
only: immediately after `complete(&cpu_running)`, immediately before
`cpu_startup_entry`, immediately before CPU0 `__cpu_up` return.

What this capture still cannot see: output that moved solely to `ttyGS1`
after earlycon, and any CPU1 progress that emits no printk. `keep_bootcon`
and `skip boot console de-registration` were present, so a silent
USB-JTAG is evidence against further *earlycon* lines, not a proof that
both harts halted.

## 120 s classification on C+0037 triple (2026-08-18T15:00:48Z)

One coherent flash of shim+Image+DTB (not Image-only). Offsets `0x10000` /
`0x90000` / `0xf10000`, `verify_flash` passed, then the same
`esptool default_reset/hard_reset chip_id` + `serial.Serial(COM10, 115200)`
path as the L2-HW 120 s run.

| Artifact | SHA-256 |
|---|---|
| boot-shim | `ceba465c1f046f409548dbbf4fe09a07ea7d9a10967e61e785e115a0363680f0` |
| Image | `700aa6db636adf34377c9fee01c682510bea4265df632ad1592adb251c79da66` |
| DTB | `a76df56172cc30a5d7f8b20d3dd8b24f66f6e67518ccf87a7d30d4240b8f639b` |

| Field | Value |
|---|---|
| File | `C:\Users\developer\tmp\easystick-m25-smp-20260817\c68-0037-120s.bin` |
| Bytes | 4559 |
| SHA-256 | `6952b6062e2ab1da5e7d9b1a33a856b43dea3e012582543ea06b59cf92934596` |
| First byte | 0.771 s |
| Port opens | 1 |
| Window | 120 s |

Hits: `C68 WAIT`, `C68 RELEASE`, `C67 UP`, `C67 CALLIN`, `C67 ONLINE`,
`smp.c:176` WARN, `C67 START cpu=1 ret=0`, **`C67 COMPLETE`**,
**`C67 UP_RETURN`**, **`C67 STARTUP`**. Misses: `Brought up`, init,
panic, watchdog, soft lockup, RCU stall.

Observed order after ONLINE:

```text
C67 START cpu=1 ret=0
C67 COMPLETE cpu=1
C67 UP_RETURN cpu=1
C67 STARTUP cpu=1
```

No further USB-JTAG bytes for the rest of the 120 s window. The previous
outcome-3 stop (ONLINE without `complete`) is **closed on this Image**.
CPU0 left `__cpu_up()` with `ret=0`. CPU1 emitted the printk immediately
before `cpu_startup_entry()`, so the stop is **in or after**
`cpu_startup_entry()` with no later earlycon line. L2.5 remains open:
`smp.c:176` is present and `Brought up … 2 CPUs` is absent.

Kernel command line still has `keep_bootcon` and
`skip boot console de-registration`.

## 120 s classification on A' IPI-provider triple (2026-08-19T00:18:19+09:00)

One coherent flash of the boot-shim, Image, and DTB was verified at
`0x10000` / `0x90000` / `0xf10000`, followed by the same
`esptool default_reset/hard_reset chip_id` and `serial.Serial(COM10, 115200)`
capture path.

| Artifact | SHA-256 |
|---|---|
| boot-shim | `1d10b96dde82069dd0cae2acd9377ee33c2caa3837bd46f94a92f7a255886439` |
| Image | `72a6c31dc12f24782d65aaedbee3618dd83854b0a116a727099b04efd14bb395` |
| DTB | `8eb409bbd50c9c0455038463a7396446f9826d00789b3b7ecbef5b826dc90830` |

| Field | Value |
|---|---|
| File | `C:\Users\developer\tmp\easystick-m25-smp-20260817\c68-a-prime-20260819-120s.bin` |
| Bytes | 15303 |
| SHA-256 | `4faf08acdc9e954e396d3faa1ff623fc2dc8e0cdcedd64017b61ecf6ac0ccf5b` |
| First byte | 0.792 s |
| Port opens | 1 |
| Window | 120 s |

**A' IPI gate: PASS on hardware.** `esp32p4-ipi: providing IPIs` appeared,
`smp.c:176` was absent, and `Brought up 1 node, 2 CPUs` appeared. All C67
markers through `C67 STARTUP` were present.

**Timer gate: FAIL / not yet implemented in this triple.** `Run /sbin/init`
and `init[` were absent. RCU reported
`Possible timer handling issue on cpu=1` and
`rcu_sched detected stalls`; no panic, watchdog, or soft lockup was observed.
The next coherent experiment is therefore 0042: SYSTIMER target1/source54
per-hart clockevent delivery to CPU1. This result is not L3 evidence.

## 120 s classification on initial CPU1-SYSTIMER triple (2026-08-19T02:16:43+09:00)

One coherent flash of the boot-shim, Image, and DTB was verified at
`0x10000` / `0x90000` / `0xf10000`, followed by the same
`esptool default_reset/hard_reset chip_id` and `serial.Serial(COM10, 115200)`
capture path.

| Artifact | SHA-256 |
|---|---|
| boot-shim | `110ee1f3effa19e18ce5dfb516bda033985c3128db30bfc0ad1d57f5af168516` |
| Image | `7c23dd11b88d70eb925865300974621982c2e7b49fcaf25ed6d1610cdfe4f062` |
| DTB | `a436f37ec18caee0d5b594e8bcaffb2ab61a81d52b16540e4d423803366aae78` |

| Field | Value |
|---|---|
| File | `C:\Users\developer\tmp\easystick-m25-smp-20260817\c68-timer-20260819-120s.bin` |
| Bytes | 3735 |
| SHA-256 | `c551330da3308e6532b8ed95b097ebeb338504d21e760c035541b29853a94cbf` |
| First byte | 0.774 s |
| Port opens | 1 |
| Window | 120 s |

**IPI gate: PASS again on hardware.** `esp32p4-ipi: providing IPIs` appeared
and `smp.c:176` was absent. `C67 COMPLETE` and `C67 UP_RETURN` were present.

**CPU1 timer gate: FAIL.** The new SYSTIMER registration line appeared:
`SYSTIMER @ 16000000 Hz, CPU0 IRQ 17/target0, CPU1 IRQ 23/target1`.
However, `C67 STARTUP`, `Brought up 1 node, 2 CPUs`, `Run /sbin/init`, and
`init[` were absent. The last ordered C67 markers were
`C67 COMPLETE` followed by `C67 UP_RETURN`; the previous `C67 STARTUP` marker
was therefore lost. Unlike the A' IPI-only capture, no RCU stall was printed
within the 120-second window.

Source audit of the emitted 0042 implementation found the cause: `COMP0_LOAD`
and `COMP1_LOAD` are separate write-one-trigger registers, whose trigger is
bit 0 in each register. The initial patch wrote `BIT(cpu)` to the selected
register, so CPU1 wrote `BIT(1)` to `COMP1_LOAD` and did not load target 1.
The patch was corrected to write `1`, and the C68 gate now rejects the
target-mask form before a replacement triple can be staged. This capture is
not L2.5 or L3 evidence.

## 120 s classification on 0045+0046 context-observe triple (2026-08-19T06:44:34+09:00)

One coherent flash of the boot-shim, Image, and DTB was verified at
`0x10000` / `0x90000` / `0xf10000`, followed by the same
`esptool default_reset/hard_reset chip_id` and `serial.Serial(COM10, 115200)`
capture path. The C68 manifest included both the 0045 SYSTIMER lock and the
0046 CPU1 context/illegal-trap observer.

| Artifact | SHA-256 |
|---|---|
| boot-shim | `3c5c0204b6320e3308fedabf734fa570fdd07cc0198c6f18896daaf19d166dbc` |
| Image | `2a000cd8413ce0dc4f9653a9ca52a7193e0f8faf337ec8396aa89b831f7790aa` |
| DTB | `675a04475b8671bcbbc01830c5927c1e5fd9c47f3382cb289a10a5e8b8519ee0` |
| staged 0045 | `b47f28425d3041b5f65462c1097005696510bf2b2b2c2bf0def33c20c7b75586` |
| staged 0046 | `a406a5ac473df713c70a88cfe5e6a8bea66ee1ea8de091df63b495764b3bbe61` |

| Field | Value |
|---|---|
| File | `C:\Users\developer\tmp\easystick-m25-smp-20260817\c68-context-observe-20260819-120s.bin` |
| Text | `C:\Users\developer\tmp\easystick-m25-smp-20260817\c68-context-observe-20260819-120s.txt` |
| JSON | `C:\Users\developer\tmp\easystick-m25-smp-20260817\c68-context-observe-20260819-120s.json` |
| Bytes | 34516 |
| SHA-256 | `45f947cb613a4b7cf1fc4628dc977dffe979f50f5be03522b8fe99734c4afe60` |
| First byte | 0.784 s |
| Port opens | 1 |
| Window | 120 s |

**L2.5: PASS on hardware for this observation triple.** The capture contains
`esp32p4-ipi: providing IPIs`, no `smp.c:176`, all C67 markers through
`C67 STARTUP`, `smp: Brought up 1 node, 2 CPUs`, `Freeing unused kernel image`,
and `Run /sbin/init`. CPU1 context switches are observed from
`swapper/1` through `migration/1`, `cpuhp/1`, `ksoftirqd/1`, `kdevtmpfs`,
and `kthreadd`. CPU1 SYSTIMER IRQ 23 reaches `actual_cpu=1` through at least
`#16384`; bidirectional IPI observations reach at least `#2048`. No
`OBS cpu1-illegal`, RCU stall, kernel panic, or soft lockup appears in the
120-second window.

This does **not** close L3. The capture does not contain the required
`/sys/devices/system/cpu/online`, `/proc/cpuinfo`, CPU1-affine work, or
explicit reverse `smp_call_function_single()` completion evidence. `init[`
is also absent, although the kernel reached `/sbin/init` and the test-only
SSH shell marker is present.

The result is an observation/perturbation result, not proof that 0046 fixes the
underlying fault. 0046 adds `pr_info()` inside `context_switch()` and can alter
the race window. The prior 0045-only control
`c68-timer-lock-20260819-120s.bin` contains
`ES_HOSTKEY GEN_BEGIN` followed by CPU1 illegal execution at `0x49bfc50c`
with `tp=0`, then `rcu_sched detected stalls` and the CRED panic. The next
controlled build should retain 0045 but remove the context-switch printk,
leaving only a low-overhead trap/context snapshot, to distinguish a real
stabilization from printk timing.

The JSON classifier's `watchdog=true` is a false positive: it matched the
ordinary `watchdogd` task name in a context-switch line. No watchdog reset
signature occurred. The old rootfs also still emitted
`esp32_sdio: version magic '6.18.35 riscv' should be '6.18.35 SMP riscv'`;
the Linux-only C68 path did not rebuild or flash rootfs, so a full Buildroot
rebuild is required before treating the network module as part of a coherent
SMP image.

## 120 s classification on the first full 0045+0046 quadruple (XZ rootfs failure, 2026-08-19)

The boot-shim, Image, rootfs and DTB were written together and each was
verified at `0x10000`, `0x90000`, `0x810000` and `0xf10000`. The Linux
artifacts were the same 0045+0046 source as the preceding observation triple.
The rootfs module was verified offline as `6.18.35 SMP riscv`.

| Artifact | SHA-256 |
|---|---|
| boot-shim | `0a8036929ab382afb4056a9f51244f849ccce54a43550270ad867b693a12f93d` |
| Image | `2d447e9990ef6d8002ebde9e5b4989bca6f08eeecd401efb63ca7caf3948561b` |
| rootfs | `27b5988e073c216e6ac5cd0cf5abab3fb5ccbdea46e2cfcdf77b83a3391ed9ba` |
| DTB | `675a04475b8671bcbbc01830c5927c1e5fd9c47f3382cb289a10a5e8b8519ee0` |

The capture was
`C:\Users\developer\tmp\c68-0045-0046-module-20260819\c68-0045-0046-module-20260819-120s.bin`,
SHA-256
`32b53782c961ff6d2a3db2e1ec98af2425f8231db6c87b9d3f277b0b094af9b7`,
403965 bytes, one port open, first byte at 0.782 s.

SMP bring-up reached `smp: Brought up 1 node, 2 CPUs`, but the rootfs did
not mount. The log reported `Filesystem uses "xz" compression. This is not
supported`, followed by
`VFS: Unable to mount root fs` and a kernel panic. The kernel configuration
has `CONFIG_SQUASHFS_LZ4=y` and no `CONFIG_SQUASHFS_XZ`; the manually
repacked image had accidentally used `mksquashfs`'s XZ default instead of
Buildroot's configured `-comp lz4`. This is an artifact-format failure, not
an SMP or module-vermagic result, and this capture is not L2.5 evidence.

## 120 s classification on the corrected 0045+0046 quadruple (LZ4 rootfs, 2026-08-19T23:04:42Z)

The same boot-shim, Image and DTB were flashed again together with a rootfs
created using Buildroot's exact SquashFS settings: LZ4, 128 KiB blocks and
`-Xhc`. All four `verify_flash` operations passed.

| Artifact | SHA-256 |
|---|---|
| boot-shim | `0a8036929ab382afb4056a9f51244f849ccce54a43550270ad867b693a12f93d` |
| Image | `2d447e9990ef6d8002ebde9e5b4989bca6f08eeecd401efb63ca7caf3948561b` |
| rootfs (LZ4) | `424a59d74286ef3cb7ab557169277b8361e589c7ef673aec74e193296dce67b6` |
| DTB | `675a04475b8671bcbbc01830c5927c1e5fd9c47f3382cb289a10a5e8b8519ee0` |

The capture was
`C:\Users\developer\tmp\c68-0045-0046-module-20260819\c68-0045-0046-module-20260819-lz4-120s.bin`,
text
`C:\Users\developer\tmp\c68-0045-0046-module-20260819\c68-0045-0046-module-20260819-lz4-120s.txt`,
and JSON
`C:\Users\developer\tmp\c68-0045-0046-module-20260819\c68-0045-0046-module-20260819-lz4-120s.json`.
It is 36743 bytes, SHA-256
`6452f0164b3637d4a9314390f484b99fc9ef3c559ce751406b2e83638bbcd87b`,
with one port open and first byte at 0.782 s.

**L2.5: PASS on hardware for the corrected 0045+0046 observation
quadruple.** The capture contains all C67 markers through `C67 STARTUP`,
`esp32p4-ipi: providing IPIs`, no `smp.c:176`, and
`smp: Brought up 1 node, 2 CPUs`. It reaches `Freeing unused kernel image`,
`Run /sbin/init` and `ES_SSH SHELL_ENTER`. CPU1 SYSTIMER IRQ 23 reaches at
least `#16384`, and bidirectional IPI observations reach at least `#2048`.
The corrected `esp32_sdio.ko` loads and creates
`/proc/esp32_sdio_h2c_obs`; no version-magic warning appears.

No `OBS cpu1-illegal`, RCU stall, kernel panic, soft lockup or watchdog reset
appears in the 120-second window. `init[` is absent because this capture
reaches the test shell path rather than recording that classifier token.
This still does **not** close L3: CPU1-affine user work and explicit
reverse-call completion evidence remain unrecorded. The result is also not
proof that 0046 fixes the underlying race, because its
`context_switch()` `pr_info()` changes timing. The next control remains
0045+0047, which retains only the low-overhead CPU1 illegal-trap observer.

## 120 s classification of the 0044 timer-observation quartet (2026-08-19)

The 0043 CLIC peer-window experiment was absent. The selected kernel profile
contained 0038, 0039, 0040, 0041, 0042 and 0044; `EASYSTICK_C68_TIMER_LOCK`
was `0`. The boot-shim, Image, LZ4 rootfs and DTB were flashed together and
each was verified at `0x10000`, `0x90000`, `0x810000` and `0xf10000`.

The four staged input hashes were:

- boot-shim: `c7ec1d122f6d067945402e006188996c88ca9f14fa550061e619b59f695110d7`
- Image: `c6605d41c6b129a79a0654f1aa38798bc4d2fc5b9c064f7a6c93cedea925d7f5`
- rootfs: `424a59d74286ef3cb7ab557169277b8361e589c7ef673aec74e193296dce67b6`
- DTB: `675a04475b8671bcbbc01830c5927c1e5fd9c47f3382cb289a10a5e8b8519ee0`

The flash-and-capture record is
`C:\Users\developer\tmp\easystick-m25-smp-20260817\c68-0044-observe-20260819-120s.json`.
The raw capture is
`C:\Users\developer\tmp\easystick-m25-smp-20260817\c68-0044-observe-20260819-120s.txt`;
it is 34,628 bytes with SHA-256
`a4b0185a471828c6164c56c9a79a6d6e64890c1cb95305cc32a3f7808c39a4b4`.
The serial port opened once and the first byte arrived at 0.770 s.

### Hardware observations

The C68 path reached every expected CPU1 startup marker:
`C67 UP`, `C67 START`, `C67 CALLIN`, `C67 ONLINE`, `C67 COMPLETE`,
`C67 UP_RETURN` and `C67 STARTUP`. Linux then reported
`smp: Brought up 1 node, 2 CPUs`; `esp32p4-ipi: providing IPIs` was present;
`smp.c:176` was absent.

The 0044 readbacks close the hardware interrupt chain for the observed timer:

- `INTMTX1=0x00000017` at `INTMTX1_OFF=0xd8`, so source 54 routes to CLIC
  slot 23 on the fixed CPU1 bank.
- CPU1's current-core CLIC slot 23 read `IP=1`, `IE=1`, `ATTR=0xc0`,
  `CTL=0x1f`. This is the local window at `0x20801000 + 23 * 4`; no
  `+0x10000` peer-window mapping was used.
- The target-1 raw interrupt was observed as `RAW=0x00000002` and
  `ST=0x00000002` before the handler, then `RAW=0` and `ST=0` after the
  clear.
- The counter chain progressed from `arm1=1` through repeated
  `IRQ -> event_handler -> rearm` cycles. At the final power-of-two
  observation it was `arm1=16385 irq1=16384 event1=16384 rearm1=16384`.

Therefore the 0042+0044 evidence rules out the following as the primary
failure in this run: missing CPU1 clockevent arm, wrong fixed CPU1 INTMTX
bank, wrong route number, CPU1 local CLIC unmask, delivery to the Linux
handler, target-1 clear, or the immediate `set_next_event()` rearm.

### Userspace boundary and verdict

This run did **not** reach the L2.5 userspace condition. The kernel registered
SquashFS and emitted `check access for rdinit=/init failed`, but the 120 s
capture contains neither `VFS: Mounted root`, `Run /sbin/init`, nor
`ES_SSH SHELL_ENTER`. It also contains no `VFS: Unable to mount root fs`,
kernel panic, RCU stall, soft lockup, or watchdog-reset signature.

The L2.5 verdict for this quartet is therefore **NOT PASS**: CPU1 SMP startup
and the target-1 timer loop pass, but init continuation was not observed.
The absence of an RCU warning is not a timer-success result here, because the
system stopped before the same userspace/RCU phase reached by the earlier
0045+0046 capture. The LZ4 image itself is a known-good format and was
verified offline; this run's missing mount completion is a separate
CPU0/rootfs-transition symptom, not evidence of the earlier XZ compression
failure.

One additional clue is that the last emitted CPU0 `set-next` power-of-two
snapshot is `#128`, while CPU1 continues through `#16384`. 0044 has no final
CPU0 counter, so this is a localization clue rather than a closed CPU0
diagnosis. The next control should retain the 0044 readbacks and enable the
0045 `ST_CONF`/`ST_INT_ENA` lock, then repeat the same quartet and rootfs
mount gate.

The full Buildroot regeneration attempted for this profile stopped at the
existing Dropbear patch `0002-easystick-ssh-postexec-markers.patch`:
`src/packet.c` hunk 1 failed while hunks 2 and 3 applied. Consequently, the
flashed rootfs is the previously Buildroot-generated LZ4 image with an
SMP-compatible module, not a newly completed 0044-specific Buildroot
rebuild. The four flash inputs are coherent and individually verified, but
source-to-rootfs regeneration remains an open gate.

## 120 s classification of the 0044+0045 timer-lock quartet (2026-08-19)

0045 was selected together with 0044, with all other C68 feature toggles
disabled. The first build attempt exposed a stale patch context: 0045 still
expected the pre-0044 `cpu1_timer_irq_count` declaration. Its hunk was
updated to anchor on the four 0044 counter arrays, then the fail-closed build
passed with `patch_0045_staged` present in the manifest.

The staged quartet was:

- boot-shim: `c7ec1d122f6d067945402e006188996c88ca9f14fa550061e619b59f695110d7`
- Image: `0df2840668ac358fa12b443bdb13c7d1a7af497f8bacb654c24e82824ccd47ed`
- rootfs: `424a59d74286ef3cb7ab557169277b8361e589c7ef673aec74e193296dce67b6`
- DTB: `675a04475b8671bcbbc01830c5927c1e5fd9c47f3382cb289a10a5e8b8519ee0`

The 0045 manifest records the staged patch hash
`b1e3444e098ff307c9f0021378fcef2195362ee0fbd0fdbb7732f0f7f9748b0b`.
All four artifacts were flashed and verified at the same offsets as the
0044 run. The capture record is
`C:\Users\developer\tmp\easystick-m25-smp-20260817\c68-0045-observe-20260819-120s.json`;
the raw text is the same basename with `.txt`. It is 70,770 bytes with
SHA-256
`0cac6ca1e770aced79799c55dd6601b78940b07dab177171f55ded75a78bfa69`,
with one port open and first byte at 0.793 s.

0045 changed the differential result materially. The first boot mounted the
same LZ4 rootfs, reached `Run /sbin/init` and entered `ES_SSH SHELL_ENTER`.
CPU0 `set_next_event()` observations continued through at least `#4096`
before the reset, instead of stopping near the 0044 run's last visible
`#128` snapshot. CPU1 retained the complete timer chain, reaching
`arm1=16385 irq1=16384 event1=16384 rearm1=16384`; target-1 readback remained
`RAW=0x2/ST=0x2` before the handler and cleared after it. The CLIC local
readback still showed `IE=1`, with CPU1 INTMTX route `0x17`.

The first boot then ended with an actual hardware reset:
`rst:0x7 (HP_SYS_HP_WDT_RESET)` and
`boot.esp32p4: CPU has been reset by WDT`. A second Linux boot began inside
the 120 s capture, so the repeated C67, mount and init lines are two boot
attempts, not a single stable 120 s run. The capture contains no RCU stall,
soft-lockup or kernel-panic line, but that absence cannot close the timer
gate after a WDT reset.

During the second boot, after repeated `ES_SSH SHELL_ENTER` markers, Linux
also emitted `WARNING: CPU: 1 PID: 0 at kernel/fork.c:736` with the saved
CPU1 context and `cause=38000003`. This is a kernel warning rather than a
panic, but it makes the WDT result a live CPU1 execution fault to classify;
0047 is the next run's low-overhead observer for exactly this boundary.

**L2.5 verdict: NOT PASS, with a strong 0045 positive differential.** The
shared `ST_CONF`/`ST_INT_ENA` lock restores CPU0/rootfs/init progress and
removes the 0044 pre-userspace stop in this A/B comparison, but the HP WDT
reset still violates the required continuous init window. This is evidence
for the lock as the next functional baseline, not proof that the underlying
SMP race is closed and not an L3 result. The next control is 0045 plus the
low-overhead 0047 illegal-trap observer, without 0046's scheduler
`context_switch()` printk.

## 120 s classification of the 0045+0047 low-overhead control (2026-08-19)

This control retained 0044's SYSTIMER observations and 0045's
`ST_CONF`/`ST_INT_ENA` raw spinlock, while adding 0047's low-overhead CPU1
illegal-trap observer. The timing-invasive 0046 `context_switch()` printk was
absent. The fail-closed build completed with 0047 staged; the quartet was
flashed and each input was verified at `0x10000` / `0x90000` / `0x810000` /
`0xf10000`.

The staged input hashes were:

- boot-shim: `c7ec1d122f6d067945402e006188996c88ca9f14fa550061e619b59f695110d7`
- Image: `6bee6c79b9f9e5cd03a5dc39ef2ba0931063739a7e25d6c1c4ace6d09dd95bc6`
- rootfs: `424a59d74286ef3cb7ab557169277b8361e589c7ef673aec74e193296dce67b6`
- DTB: `675a04475b8671bcbbc01830c5927c1e5fd9c47f3382cb289a10a5e8b8519ee0`
- staged 0045: `b1e3444e098ff307c9f0021378fcef2195362ee0fbd0fdbb7732f0f7f9748b0b`
- staged 0047: `d5f3c0e32a3f2d530cc5127c8f6af14c55edde7d41ea87dc6b3d09a11552f1af`

The flash-and-capture record is
`C:\Users\developer\tmp\easystick-m25-smp-20260817\c68-0045-0047-observe-20260819-120s.json`;
the raw text is the same basename with `.txt`; the binary is the same
basename with `.bin`. The capture is 38,531 bytes with SHA-256
`96ebcf939c4b0efa9f9b43294fc29a9f7a18b1d6f61f09dcfef06a81b8f4d2cf`, one
serial-port open, and first byte at 0.760 s.

The measured UART evidence is coherent:

- CPU1 reaches all C67 markers through `C67 STARTUP`, Linux reports
  `smp: Brought up 1 node, 2 CPUs`, and `smp.c:176` is absent.
- The fixed CPU1 INTMTX route remains `INTMTX1=0x00000017`; CPU1 local CLIC
  slot 23 reads `IP=1 IE=1 ATTR=0xc0 CTL=0x1f`.
- CPU1 SYSTIMER continues through
  `arm1=16385 irq1=16384 event1=16384 rearm1=16384`; the observed
  pre-handler `RAW/ST=0x2` clears to zero after the handler.
- Bidirectional IPI observations continue in both directions, including
  `ipi-irq` and `ipi-post` on CPU0 and CPU1.
- The same boot reaches `VFS: Mounted root`, `Run /sbin/init`, and repeated
  `ES_SSH SHELL_ENTER` markers. The CPU0 `set_next_event()` observations
  reach at least `#16384`.
- `OBS cpu1-illegal` is absent. No `WARNING:`, RCU stall, soft lockup,
  kernel panic, `HP_SYS_HP_WDT_RESET`, or `CPU has been reset by WDT` appears
  in the 120-second capture. `ES_WDT WDT_FEED_COUNT` advances through 8,
  which is consistent with continued system progress.

**L2.5 verdict: PASS on hardware for the 0045+0047 control.** It satisfies
the contract's SMP/IPI/no-`smp.c:176`/CPU1-startup/init-continuation criteria
for a clean 120-second run. This is the first control in this sequence to
retain 0045 while removing 0046's timing perturbation and still avoid the
0045-only WDT reset.

The result does not close L3. It does not yet record the required
CPU1-affine userspace work, `/sys/devices/system/cpu/online`,
`/proc/cpuinfo`, or explicit reverse `smp_call_function_single()` completion.
It also does not prove that 0047 itself fixes the fault: the 0045+0047 Image
is a new build, and the absence of the previous `cause=0x38000003` fork WARN
is an observation, not a causal isolation. The next step is therefore an L3
smoke test on this stable baseline, while preserving the same 0044/0045/0047
observers and capture manifest.

## 120 s classification of the 0045+0047+0048+0049 L3 smoke profile (2026-08-19)

The L3 profile retained the low-overhead 0047 observer, added the renamed
`m5stamp` endpoint in 0048, and added the workqueue reverse-IPI path in 0049.
The boot-shim, Image, rootfs, and DTB were built as one C68 profile and
flashed together at `0x10000`, `0x90000`, `0x810000`, and `0xf10000`.

| Artifact | SHA-256 |
|---|---|
| boot-shim | `c7ec1d122f6d067945402e006188996c88ca9f14fa550061e619b59f695110d7` |
| Image | `21541b5f4b89441658d7133f02904c988a896cb56c44eebc272f6ad33167a24d` |
| rootfs | `0c96e2d14b51a90a4e414326098895bca41272c12040b9ce550075d350e5c557` |
| DTB | `675a04475b8671bcbbc01830c5927c1e5fd9c47f3382cb289a10a5e8b8519ee0` |
| staged 0045 | `b1e3444e098ff307c9f0021378fcef2195362ee0fbd0fdbb7732f0f7f9748b0b` |
| staged 0047 | `d5f3c0e32a3f2d530cc5127c8f6af14c55edde7d41ea87dc6b3d09a11552f1af` |
| staged 0048 | `2bd7519f0558a7423f48078df177ce1963af4f130ce4925cefe23aac8b0e55a6` |
| staged 0049 | `c76c255b23b8959473ff7388930e4a830002041c830bb4f0efab9f7197915201` |

The clean retry record is
`C:\Users\developer\tmp\easystick-m25-smp-20260819\c68-0045-0047-l3-20260819-workqueue-pass-120s.json`;
the raw UART capture has the same basename with `.txt` and `.bin`.
The capture is 38,969 bytes with SHA-256
`e7ed05f53354a02961d8c00f77d7a3c7fdd1bfb0e9ad15227e77a76d7dab1187`,
one serial-port open, and first byte at 0.771 s.

**L3 smoke: PASS for this clean 120 s hardware run.** The raw UART evidence
contains:

- `smp: Brought up 1 node, 2 CPUs`, `Run /sbin/init`, and all C67 markers
  through `C67 STARTUP`;
- `/usr/sbin/m5stamp-smp-smoke`;
- `M5STAMP_L3SMOKE topology online=0-1 processors=2`;
- CPU1-affine work with `cpu=1`;
- `crosscall status=PASS ... forward_cpu=1 reverse_init_cpu=1
  reverse_cpu=0 ... forward_rc=0 reverse_init_rc=0 reverse_target_rc=0`;
- `M5STAMP_L3SMOKE PASS`.

The same capture has no `smp.c:176`, `WARNING:`, CPU1-illegal, RCU stall,
soft lockup, kernel panic, Guru Meditation, or HP WDT reset marker. This
closes the requested L3 smoke criteria for one bounded run; it does not
advance the project to manufacturing or merge SMP into the default UP image.

An immediately preceding capture of the **same four artifact hashes** is
preserved as
`C:\Users\developer\tmp\easystick-m25-smp-20260819\c68-0045-0047-l3-20260819-workqueue-nohelper-120s.json`.
That attempt had `command_sent=false`, three observed boots, CPU1-illegal,
and WDT/Guru Meditation markers. The successful retry therefore proves a
clean run, not repeatability of the startup race; repeat captures remain a
follow-up before any release-state change.

The source and current generated rootfs audit found no legacy
smoke-helper marker. One older pre-rename capture JSON under the external
`easystick-m25-smp-20260817` evidence directory still contains the legacy
helper path; it is retained as historical evidence and is not an input to the
current image.

## Next repeatability gate: C68 bounded process stress

The next hardware experiment is a new C68 image selected with
`EASYSTICK_C68_L3_SMOKE=1 EASYSTICK_C68_STRESS=1`. It adds the C68-only
BusyBox `top` configuration and `/usr/sbin/m5stamp-smp-stress`; neither is
present in the UP profile. The helper is bounded by an explicit
`--seconds 90` argument inside the fixed 120 s UART capture window and
reports:

- CPU topology and CPU0/CPU1 affinity;
- a persistent CPU1 worker plus repeated `fork`/`exec` of short CPU1 workers;
- bounded `malloc`/touch/free activity;
- repeated `/proc/m5stamp_smp_smoke` bidirectional IPI calls;
- non-interactive `top -b -n 1` snapshots at the start, during the run, and
  at the end.

The capture script now emits schema
`easystick.c68-capture/v2`, ordered boot events, command-send timing,
helper/top lines, and a conservative classification. A stress capture is
not a PASS until the raw UART, capture JSON, and all four flash-input hashes
are retained together. The first retained result is recorded below.

## Bounded process stress result (2026-08-19)

The corrected isolated M1 image was built after the first attempt failed
closed on a stale `S40network` file left in the shared Buildroot target. The
final rootfs post-build boundary removes network-only files for non-network
profiles, and the profile gate also rejects stale Dropbear, M5Stamp smoke, and
M5Stamp stress helpers. The image passed the C68 gates with SMP, the
USB Serial/JTAG ACM console, `top`, and the stress helper enabled.

| Artifact | SHA-256 |
|---|---|
| boot-shim | `0fcb81b0b738600999593dbbf1c63cd12565dc4fe0ea51c09a57953376690379` |
| Image | `21d6dbdeb8b885bb8ca8409700c71ec68959e50e18ae99f133930a851c3899fd` |
| rootfs | `a3e3a2bf3c13ff304e9246d5cb56bb2a96d2409554cd9946cc0d324293a8c6da` |
| DTB | `675a04475b8671bcbbc01830c5927c1e5fd9c47f3382cb289a10a5e8b8519ee0` |

The flash-and-capture record is
`C:\Users\developer\tmp\easystick-m25-smp-20260817\c68-0045-0047-l3-stress-20260819-m1-isolated-v2-90helper-120s.json`;
the raw UART files use the same basename with `.txt` and `.bin`. The capture
is schema `easystick.c68-capture/v2`, 62,351 bytes, SHA-256
`6dc109d14ad7a708bdd1255bad2c3e6d3d7f5b4b923607e7c793f046a6903bfc`, one
COM10 open, and first byte at 0.256 s. All four flash inputs passed
`SHA_OK`, `FLASH_AND_VERIFY_OK` was observed, and the command was sent at
2.595 s after the askfirst console was activated.

### Hardware observations

The raw capture reached `C67 UP`, `START`, `CALLIN`, `ONLINE`, `COMPLETE`,
`UP_RETURN`, `STARTUP`, `smp: Brought up 1 node, 2 CPUs`, and
`Run /sbin/init`. It contains no `smp.c:176`, `WARNING:`, CPU1-illegal,
RCU stall, soft lockup, kernel panic, Guru Meditation, or HP WDT reset
marker.

The stress helper reported:

```text
M5STAMP_STRESS topology online=0-1 processors=2 seconds=90
M5STAMP_STRESS WORKER_START cpu=1 seconds=90
M5STAMP_STRESS WORKER_STOP pid=45 status=0
M5STAMP_STRESS PASS seconds=90 iterations=262 top_snapshots=5 worker_execs=262 smoke_calls=262
```

The capture contains five complete `M5STAMP_TOP BEGIN`/`END` blocks
(`start`, iterations 88, 175, 262, and `end`). CPU1 SYSTIMER counters
continued through `arm1=4096 irq1=4095 event1=4095 rearm1=4095`, and the
bidirectional IPI observer reached both send and IRQ/post markers. The
capture classifier is **`helper-pass`**.

This is a clean bounded process/SMP/top PASS on the current isolated image.
It does not merge SMP into the default UP image. The immediately preceding
same-image capture used a 120-second helper in a 120-second window; it had
topology, CPU1 worker, timer/IPI, and top evidence with no fault marker, but
the window closed before the helper emitted its final PASS. The capture
classifier now reports that condition as `stress-incomplete`, and the default
command leaves a 30-second completion margin.

## Repeat bounded process stress capture (2026-08-19)

A second capture used the same four flash-input hashes without a source or
image change. The record is
`C:\Users\developer\tmp\c68-0045-0047-l3-stress-20260819-m1-isolated-v2-repeat2-120s.json`;
the raw UART files use the same basename with `.txt` and `.bin`. It is
62,340 bytes with SHA-256
`928dd5a4af1cb97c850727216d4da0611fb64e096b20d2ba8b418164f0c7d9ee`, one
COM10 open, and first byte at 0.253 s. `SHA_OK` and `FLASH_AND_VERIFY_OK`
again passed, and the command was sent at 2.333 s.

The second helper reported:

```text
M5STAMP_STRESS topology online=0-1 processors=2 seconds=90
M5STAMP_STRESS WORKER_STOP pid=44 status=0
M5STAMP_STRESS PASS seconds=90 iterations=263 top_snapshots=5 worker_execs=263 smoke_calls=263
```

It again emitted five complete top blocks and showed no `smp.c:176`,
warning, CPU1-illegal, RCU stall, soft lockup, panic, Guru Meditation, or
WDT marker. The classifier was **`helper-pass`**. Two consecutive
same-input helper-pass captures close the short repeatability gate for this
image. Longer soak, host/temperature coverage, and default-profile promotion
remain separate release gates.

## Pre-mitigation same-input startup retry (2026-08-19)

A third capture used the exact four flash-input hashes above after the
Buildroot output volume had been removed and the inputs were preserved outside
that disposable volume. The record is
`C:\Users\developer\tmp\easystick-m25-smp-20260817\c68-0045-0047-l3-stress-20260819-m1-isolated-v2-r4-120s.json`;
the raw UART files use the same basename with `.txt` and `.bin`. It is schema
`easystick.c68-capture/v2`, 89,538 bytes with SHA-256
`a7b6e202ba50b9a54fde7d340c6eabc5b4ee43bfa1798e20058b755738109e67`, one
COM10 open, and first byte at 0.262 s. All four inputs passed `SHA_OK`, and
`FLASH_AND_VERIFY_OK` was observed.

The image reached `Brought up 1 node, 2 CPUs` and `/sbin/init`, but the
command was not sent. The UART then recorded
`seedrng: can't create directory '/var/lib/seedrng': Read-only file system`,
an `Oops - load access fault` in `path_openat` from `sh`, `Kernel panic - not
syncing: Fatal exception in interrupt`, RCU stalls, and
`HP_SYS_HP_WDT_RESET`. No stress or `top` marker was emitted. The manifest was
generated before the classifier-order correction and says `no-command-sent`;
the raw fault markers make the corrected classification **`watchdog-reset`**.

This is the same-image reproducibility failure that the two earlier
helper-pass captures could not exclude. They remain valid positive evidence
that process execution and `top` can work, but the short repeatability gate is
**reopened** until the startup fault and command-readiness race are explained.
The host-side classifier now prioritizes WDT/panic/RCU faults over
`no-command-sent`, and its new negative tests cover this masking case.

## Corrected bounded-top image repeatability (2026-08-19)

The seedrng-free profile was rebuilt after the startup fault above, and the
stress helper was rebuilt with a 4 KiB per-snapshot `top` output cap. The
resulting `m1-topcapfix` image was flashed three times with the exact same
four inputs:

| Artifact | SHA-256 |
|---|---|
| boot-shim | `c8a41756495694df08596ff276a3731c237eb0e3e43c40c62e55054ef6238665` |
| Image | `9c7b88d735e293dab3527d27e4f971450b7e0c33e98f95521e51be55f1e8bc7c` |
| rootfs | `e0fb5870f2dce52317fd9e7f3c29a3054d84c8f2f08d8f0aede2682e692028a2` |
| DTB | `675a04475b8671bcbbc01830c5927c1e5fd9c47f3382cb289a10a5e8b8519ee0` |

### First capture

The record is
`C:\Users\developer\tmp\easystick-m25-smp-20260817\c68-0045-0047-l3-stress-20260819-m1-topcapfix-120s.json`;
the raw UART files use the same basename with `.txt` and `.bin`. It contains
62,331 bytes with SHA-256
`ce315d0cac4d10c857a3b8c4e4a639d44d98cf58be84e474982a752abacb340c`, one
COM10 open, first byte at 0.261 s, and classification `helper-pass`.

### Repeat capture

The second record is
`C:\Users\developer\tmp\easystick-m25-smp-20260817\c68-0045-0047-l3-stress-20260819-m1-topcapfix-repeat2-120s.json`;
the raw UART files use the same basename with `.txt` and `.bin`. It contains
63,781 bytes with SHA-256
`ba961721fc61a65c13b434a071bbceff43d475e0cdde00cb7de982120c807382`, one
COM10 open, first byte at 0.257 s, and classification `helper-pass`.

Both captures passed `SHA_OK` and `FLASH_AND_VERIFY_OK`, reached
`Brought up 1 node, 2 CPUs` and `/sbin/init`, and sent
`/usr/sbin/m5stamp-smp-stress --seconds 90` through the activated `ttyGS1`
prompt. Both reported:

```text
M5STAMP_STRESS topology online=0-1 processors=2 seconds=90
M5STAMP_STRESS PASS seconds=90 iterations=262 top_snapshots=5 worker_execs=262 smoke_calls=262
```

The two runs each produced five complete bounded `M5STAMP_TOP BEGIN`/`END`
blocks, CPU1 worker completion, CPU1 timer observations through
`arm1=16384 irq1=16383 event1=16383 rearm1=16383`, and bidirectional IPI
observations. Neither capture contains `smp.c:176`, a warning, CPU1-illegal
trap, RCU stall, soft lockup, panic, Guru Meditation, or WDT reset.

### Third capture

The third record is
`C:\Users\developer\tmp\easystick-m25-smp-20260817\c68-0045-0047-l3-stress-20260819-m1-topcapfix-repeat3-120s.json`;
the raw UART files use the same basename with `.txt` and `.bin`. It contains
62,407 bytes with SHA-256
`cec241494cfb8d8f2dda28259b73b2a1fe9afc577101b962263b7e3225984345`, one
COM10 open, first byte at 0.257 s, and classification `helper-pass`.

It passed all four flash-input checks, reached `Brought up 1 node, 2 CPUs`
and `/sbin/init`, sent the helper at 2.332 s, and reported
`M5STAMP_STRESS PASS seconds=90 iterations=263 top_snapshots=5
worker_execs=263 smoke_calls=263`. It emitted five complete top blocks and
contained no `smp.c:176`, warning, CPU1-illegal trap, RCU stall, soft lockup,
panic, Guru Meditation, or WDT reset.

**Short repeatability verdict: PASS for the corrected `m1-topcapfix` image.**
Three consecutive same-input captures now pass for this exact artifact set.
This does not erase the earlier pre-mitigation `m1-isolated-v2` startup fault,
and it does not close longer soak, host/temperature coverage, or promotion of
SMP into the default UP profile.
