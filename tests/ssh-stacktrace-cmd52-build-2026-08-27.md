# SSH stacktrace/CMD52 diagnostic image — 2026-08-27

Status: **P4 FLASHED — SSH command wedge reproduced with and without CMD52 printk**

## Scope and safety boundary

This candidate adds the P4-only diagnostic path:

- `EASYSTICK_STACKTRACE_DIAGNOSTICS=1`
- `CONFIG_STACKTRACE=y`
- `CONFIG_KALLSYMS=y`
- `CONFIG_KALLSYMS_ALL=y`
- `CONFIG_DEBUG_INFO=y`
- `CONFIG_DEBUG_INFO_DWARF4=y`
- `CONFIG_FRAME_POINTER=y`
- WDT pretimeout `dump_stack()` from `0056-easystick-wdt-stacktrace.patch`
- bounded CMD52 boundary records for `ESP_SLAVE_TOKEN_RDATA` from
  `0029-easystick-sdio-cmd52-boundaries.patch`

The image uses the existing P4 host-side path with
`EASYSTICK_IDMAC_NONCOHERENT_RING=1`,
`EASYSTICK_CMD53_BB_ALLOW_IDMAC_NC=1`, and `EASYSTICK_SDIO_FORCE_PIO=0`.
No C6 image, C6 flash, C6 erase, or device reset was performed by this build.

## Build and verification

- Build profile: `m3-lab`; boot-shim profile: `m2`.
- Retention BB: version `6`, size `0x120` (`288` bytes), PA `0x50108080u`.
- Diagnostic build completed with exit code `0`.
- `0056` applied with Buildroot's strict `-F0` patch mode.
- WDT crash-capsule fail-closed gate: **PASS**.
- Final-shot manifest verification with `--require-shot-c`: **PASS**.
- Exported `SHA256SUMS.txt`: all listed artifacts **PASS**.
- Built ESP-Hosted source contains `ES_CMD52` instrumentation.

The first retry exposed two build-tooling defects, both corrected before the
successful run: `0056` declared a 14-line hunk while containing 12 old lines,
and the post-build WDT gate omitted `import os`. The successful build ran
after both corrections.

## Runtime attempt

The P4-only write and verify gate passed on `COM10`; no C6 image, erase, or
write was performed. The flash script left the target in the bootloader, then
`capture-boot.ps1 -Reset` performed the single HardReset-compatible
RTS/DTR sequence before collecting UART.

### UART evidence

- Capture: `ssh-stacktrace-cmd52-runtime2-uart-capture-2026-08-27.bin`
- Size: `31,941` bytes
- SHA-256: `d4a2b23d6a6b5163747b80cb2274e5f89c121e6b8d2389d37162ac7091c527fa`
- Capture manifest:
  `ssh-stacktrace-cmd52-runtime2-uart-capture-2026-08-27.bin.json`
- Manifest: `port_opens=10`, `first_byte_seconds=2.111`,
  `control_lines_driven=true`
- Reset line: requested `rst:0x17 (CHIP_USB_UART_RESET)` only
- P4 flash write/verify: **PASS**
- Linux rootfs, Wi-Fi, static IPv4 `10.255.10.161`, and Dropbear: **PASS**
- WDT reset, kernel panic, and runtime `Call Trace`: **NOT OBSERVED**

The UART reaches:

```text
VFS: Mounted root (squashfs filesystem) readonly on device 31:0.
ES_SDIO_LOAD_OK source=S40 requested_mhz=5 module_param_mhz=5 ...
M3-lab: association complete; using static IPv4
inet addr:10.255.10.161
M3-lab: starting password-enabled Dropbear on TCP/22
```

The final CMD52 boundary records are:

```text
ES_CMD52 seq=78 stage=BYTE_ENTER addr=0x46 index=2 size=4 lock=1 ...
ES_CMD52 seq=79 stage=BYTE_DONE  addr=0x46 index=2 size=4 lock=1 value=0x3c ret=0
```

There is no following `BYTE_ENTER addr=0x47`, `BYTE_DONE addr=0x47`, or
`RELEASE_*` record, and no later UART output in the 60-second capture. The
boot-time `CRASH_CAPSULE empty/invalid` is not a dump from this SSH attempt.

### SSH evidence

- Reproduction: `ssh-stacktrace-cmd52-runtime2-repro-2026-08-27.json`
- Size: `969` bytes
- SHA-256: `72aabbb9553d53c4660c9e196b1118684c6fc141e0be7622db6fbb4e5ce1b5f1`

```text
TCP22_UP attempt=1 elapsed=0.063
SSH_AUTHENTICATED elapsed=1.125
CHANNEL_OPEN elapsed=1.266
EXEC_SENT command=id elapsed=1.297
EXIT_TIMEOUT elapsed=13.359 stdout_bytes=0 stderr_bytes=0
POST_TCP22_FAIL elapsed=16.359
```

The host-side trace proves TCP/22, SSH authentication, channel creation, and
the `exec("id")` request all succeeded. No stdout, stderr, exit status, or
EOF arrived; the subsequent TCP/22 probe timed out. UART capture has no
per-byte host timestamp, so the exact host event-to-UART latency is not
claimed. Together, the traces reproduce the same post-auth wedge and narrow
the device-side last observed CMD52 boundary to completion of byte index 2
(`addr=0x46`), before the fourth byte (`addr=0x47`) begins.

This is **SSH stability FAIL**, but it is a stronger localization than the
earlier TX-ledger capture. It does not prove whether the final stop is inside
the next CMD52 invocation, in the SDIO controller/C6 response, or after the
instrumentation's last emitted line. It also does not provide a WDT stacktrace,
because this run did not reset during the capture window.

## Export

Directory:

`D:\Users\Developer\easystick-tmp-20260820\easystick-p4-stacktrace-cmd52-20260827\`

| Artifact | SHA-256 |
|---|---|
| `bootloader.bin` | `f4a113958a7d00c8a328dfb8b9d97af6d813f9a8b6d35fbaca64de806bdbc61c` |
| `partition-table.bin` | `d076fd66f0f4bd3f9f423761ef10b73652f2359f190c5ffef0164f657c40d9d4` |
| `boot-shim.bin` | `1e5ed162b234be1c60408e70d91777ccf4c3b0e4219f56b80fa2fd0e0ecec608` |
| `Image` | `c696eccf5aeb2ea520253f53d6a529f5c4c60ff429957dd058b59a3d35aa8c98` |
| `rootfs.squashfs` | `b816bc9ae802e6602689a8cddedb48863d4d02b795f0f72a9caafc1d208be64d` |
| `easystick-stamp-p4.dtb` | `7c3bd1e042d71aef80ebbeb37cfb402cabb400e3a41dc82b4a4b5d59a18eb163` |
| `cmd53-bb-final-shot-manifest.json` | `1d88259fe3761307ff13b8e7acd53f6564d300eb52b635fb10f52432835495eb` |
| `linux-built.config` | `841efcb9e5f468d388049354889cc5aae99bc4e8b0905de03cb65fd8cc729c6d` |
| `vmlinux` | `dc8ca7a4d03f335313632e6e5eaa2c6939daffa484c755bd9b9ea6ea493c8e28` |
| `System.map` | `b4be6ba7a0270a9fdb44bab367e5647025706e321206d5f6fc284d63c33599e6` |

The final manifest records `shot_c_allowed=true`, but that is only the
retention-BB flash gate. It is not runtime evidence and does not authorize a
flash by itself. The hashes above identify the image that was flashed in this
runtime attempt.

## `ES_CMD52` printk-disabled control

The control image retained `EASYSTICK_STACKTRACE_DIAGNOSTICS=1`, WDT/stacktrace
configuration, retention BB 0052/0053/0054, DMA `NC=1`, SDIO 5 MHz,
`EASYSTICK_ESPHOSTED_DISABLE_0010=1`, and all ledger settings from runtime2.
The only diagnostic-path change was:

```text
EASYSTICK_ESPHOSTED_CMD52_TRACE=0
0029-easystick-sdio-cmd52-boundaries.patch=absent
```

- Build: **PASS**, exit code `0`; rootfs `3,076,096` bytes.
- Control `Image` SHA-256:
  `22e4e24f0672a76953c45920d2e66d4f6ebcb1d233e9222c5d7308e656c69fe0`
- Control `rootfs.squashfs` SHA-256:
  `45aeb057365463e9a65eb54c76da22f8231417eade9b2cecd3b199a2689e05e4`
- Control DTB SHA-256:
  `7c3bd1e042d71aef80ebbeb37cfb402cabb400e3a41dc82b4a4b5d59a18eb163`
- The control build output contains neither `0029` nor `ES_CMD52` /
  `easystick_cmd52_trace` markers. The WDT attestation and final-shot
  manifest gates both passed.
- P4-only flash/write and verify: **PASS** on `COM10`; preserved stock
  readback SHA-256 remained
  `229459f251eaf6222f0c07968702d72a3818e520da1788ce465027d969020c24`.
  No C6 image, erase, or write was performed.

### Control UART evidence

- Capture:
  `ssh-stacktrace-cmd52-quiet-control-uart-capture-2026-08-27.bin`
- Size: `16,284` bytes
- SHA-256:
  `89c96cf29d299418f56548344e30b7742da0f164e835158ecafa993ac962696c`
- Capture manifest:
  `ssh-stacktrace-cmd52-quiet-control-uart-capture-2026-08-27.bin.json`
- Manifest: `port_opens=11`, `first_byte_seconds=2.113`,
  `control_lines_driven=true`

The control UART reached a mounted root, Wi-Fi association with
`10.255.10.161`, and password-enabled Dropbear. In the 16,284-byte runtime
capture: `ES_CMD52=0`, WDT marker count `0`, kernel panic count `0`, and
`Call Trace` count `0`.

### Control SSH evidence

- Reproduction:
  `ssh-stacktrace-cmd52-quiet-control-repro-2026-08-27.json`
- Size: `1,042` bytes
- SHA-256:
  `3c5f15c2d41c8dc54debda0491236a396c16d2ded26a50f10fa327a879268288`

```text
TCP22_UP attempt=1 elapsed=0.093
SSH_AUTHENTICATED elapsed=0.843
CHANNEL_OPEN elapsed=0.875
EXEC_SENT command=id elapsed=0.906
EXIT_TIMEOUT elapsed=12.937 stdout_bytes=0 stderr_bytes=0
POST_TCP22_FAIL elapsed=18.953
```

The control therefore reproduces the same post-auth `exec("id")` wedge after
the `ES_CMD52` UART observer has been removed: no stdout, stderr, exit status,
or EOF arrived, and the post-command TCP/22 probe timed out. This rejects the
hypothesis that the existing `ES_CMD52` `printk/pr_emerg` output alone is the
cause. It does not provide a new `0x46`/`0x47` boundary, because the control
intentionally removes those markers; it also produced no WDT reset or
stacktrace during the capture window.

## Control export

Directory:

`D:\Users\Developer\easystick-tmp-20260820\easystick-p4-stacktrace-cmd52-quiet-20260827\`

| Artifact | SHA-256 |
|---|---|
| `boot-shim.bin` | `1e5ed162b234be1c60408e70d91777ccf4c3b0e4219f56b80fa2fd0e0ecec608` |
| `Image` | `22e4e24f0672a76953c45920d2e66d4f6ebcb1d233e9222c5d7308e656c69fe0` |
| `rootfs.squashfs` | `45aeb057365463e9a65eb54c76da22f8231417eade9b2cecd3b199a2689e05e4` |
| `easystick-stamp-p4.dtb` | `7c3bd1e042d71aef80ebbeb37cfb402cabb400e3a41dc82b4a4b5d59a18eb163` |
| `cmd53-bb-final-shot-manifest.json` | `e1bc6b83449343017bdc7d7f9093caa43340c2d4cdcc5db2da912107e2e22860` |
| `linux-built.config` | `841efcb9e5f468d388049354889cc5aae99bc4e8b0905de03cb65fd8cc729c6d` |
| `vmlinux` | `46b353f14e2466744a0a94de7183218ea166bd2386446b36d1eb5ebc7e491356` |
| `System.map` | `b4be6ba7a0270a9fdb44bab367e5647025706e321206d5f6fc284d63c33599e6` |

## CMD52 M1..M4 non-console marker candidate

The marker candidate was built after the printk-disabled control, preserving
the same runtime conditions while adding no console observer:

- Build: **PASS**, exit code `0`; `m3-lab` / boot-shim `m2`.
- Retention BB: v6, size `0x120` (288 bytes), PA `0x50108080u`.
- Marker: enabled at `0x501081a0u` (`BB_PA + 0x120`); `0030` rendered from the
  boot-shim `nm` PA with no `BBDEAD` placeholder.
- M1/M2/M3/M4 words: `0x45534d31`, `0x45534d32`, `0x45534d33`,
  `0x45534d34`.
- Runtime contract: `IDMAC_NONCOHERENT_RING=1`, explicit NC allow,
  `SDIO_FORCE_PIO=0`, `EASYSTICK_ESPHOSTED_CMD52_TRACE=0`, and no TX/TCP/SSH
  ledgers.
- Rootfs: `3,076,096` bytes; the 4 MiB rootfs-window gate passed.
- WDT crash-capsule fail-closed attestation: **PASS**.
- Final-shot manifest write and `--require-shot-c` verification: **PASS**.
- P4 flash was **not** performed for this candidate; no C6 image, erase, or
  write was performed.

Staged export:
`C:\Users\developer\tmp\easystick-p4-cmd52-marker-nc1-20260827\`

Key artifact hashes:

| Artifact | SHA-256 |
|---|---|
| `boot-shim.bin` | `97f8883fccd9c310f1c7b8d4a3224cbb65f45a091adf58e8725b63844329dee7` |
| `Image` | `b1a62b501086476b8fdfd607f13d126213f960432aca2176b785f6a0be735fac` |
| `rootfs.squashfs` | `79633a683c55a22ec7636211c827770aae60bc2925ac612730e55859402b3184` |
| `easystick-stamp-p4.dtb` | `7c3bd1e042d71aef80ebbeb37cfb402cabb400e3a41dc82b4a4b5d59a18eb163` |
| `cmd53-bb-final-shot-manifest.json` | `578dde5037d0998848292f8a28b5c9a4248297ee813e229924b7b4f6b385dcb0` |

The one-word design has a deliberate observability limit: M2 and M3 are
consecutive stores, so any ordinary path that reaches M2 overwrites it with
M3. An observed M2 therefore means execution did not complete the following
M3 store; normal last-value interpretation is M1, M3, or M4.

## Marker runtime result

The candidate was flashed to P4 `COM10` only. The flash script reported
`P4 candidate write and verify passed`; the preserved 16 MiB stock readback
remained SHA-256
`229459f251eaf6222f0c07968702d72a3818e520da1788ce465027d969020c24`.
No C6 image, erase, or write was performed.

UART-first boot capture:

- Capture:
  `ssh-stacktrace-cmd52-marker-nc1-uart-capture-2026-08-27.bin`
- Size: `15,782` bytes
- SHA-256:
  `5b7b8c585f04aaa3548187f8c3c483f9d6c6678a6af1ef651745f2da4e2a4899`
- Manifest:
  `ssh-stacktrace-cmd52-marker-nc1-uart-capture-2026-08-27.bin.json`
- `port_opens=13`, `first_byte_seconds=2.116`,
  `control_lines_driven=true`
- Rootfs mounted, Wi-Fi associated, static IPv4 `10.255.10.161` assigned,
  and Dropbear started: **PASS**
- WDT, kernel panic, and runtime `Call Trace`: **NOT OBSERVED**

SSH reproduction:

- Evidence:
  `ssh-stacktrace-cmd52-marker-nc1-repro-2026-08-27.json`
- SHA-256:
  `887cc395bf8bf609db7851619caae28631dc24df67833a2a9be76c50c491d656`

```text
TCP22_BEFORE reachable=True elapsed=0.188
SSH_AUTHENTICATED elapsed=0.969
CHANNEL_OPEN elapsed=1.0
EXEC_SENT command=id elapsed=1.032
EXIT_TIMEOUT stdout_bytes=0 stderr_bytes=0 closed=False elapsed=13.032
TCP22_AFTER reachable=False error=TimeoutError('timed out') elapsed=16.032
```

The same post-auth `exec("id")` wedge remains: no stdout, stderr, exit
status, or EOF arrived.

After the wedge, a controlled P4-only reset was used to retrieve retention:

- Capture:
  `ssh-stacktrace-cmd52-marker-nc1-marker-recovery-uart-capture-2026-08-27.bin`
- Size: `15,609` bytes
- SHA-256:
  `59bb28a60ae849e92fc7acf34385da876ede50358c60c026aba386763ed5b9ca`
- Manifest:
  `ssh-stacktrace-cmd52-marker-nc1-marker-recovery-uart-capture-2026-08-27.bin.json`
- `port_opens=2`, `first_byte_seconds=2.106`,
  `control_lines_driven=true`

The recovery UART reported:

```text
easystick-boot: CMD53_BB pa=0x50108080 reset_reason=11
easystick-boot: CMD52_MARKER last=0x00000000
easystick-boot: CMD53_BB empty/invalid ...
easystick-boot: CRASH_CAPSULE empty/invalid
```

The candidate's rendered patch and compiled module were independently checked
for `PA=0x501081a0` and the M1..M4 constants before this interpretation.
Therefore the post-reproduction recovery value contains no surviving M1, M3,
or M4 store. Under the matching `reg=0x44,size=4` predicate, this is
consistent with the current run stopping before M1, including a
`sdio_readb(0x46)` call that did not return. It does not distinguish an
earlier stop, failure to enter the matching 4-byte read, or a store-visibility
failure; it also does not prove the prior instrumented run's `0x46` return is
repeatable. The initial boot's non-marker value was stale retention content
before the boot-shim cleared it and is not part of this shot.

**SSH stability: FAIL.** The printk-disabled control already rejected the
console Heisenbug; this marker shot now shows that the same SSH wedge can
occur without a surviving non-console boundary marker. No WDT capsule or
symbolic stacktrace was produced.
