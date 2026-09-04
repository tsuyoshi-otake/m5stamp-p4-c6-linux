# CMD52 six-word marker positive-control runtime — 2026-08-28

## Scope and condition

This shot tested the six-word non-console CMD52 marker on EasyStick Stamp-P4
`COM10`. The candidate was the NC=1 `m3-lab` image with build-time Wi-Fi
provisioning, `EASYSTICK_ESPHOSTED_CMD52_MARKER=1`, and the semantically
correct `AFTER_46` placement. Only the P4 was flashed. The C6 image was not
erased or written; the preserved 16 MiB stock readback remained
`229459f251eaf6222f0c07968702d72a3818e520da1788ce465027d969020c24`.

The retention layout and marker contract used by the image were:

```text
CMD53_BB PA:       0x50108080
CMD52 marker PA:   0x501081a0
word0 MAGIC:       0x45534d30
word1 ARMED:       0x45534d31
word2 TOKEN_ENTER:  0x45534d32
word3 AFTER_46:    0x45534d33
word4 BEFORE_47:   0x45534d34
word5 AFTER_47:    0x45534d35
```

All resets below used the P4-only `capture-boot.ps1 -Reset` path. No C6
operation was part of the runtime test.

## Evidence

The raw captures and SSH event log are retained outside Git at:

`C:\Users\developer\tmp\easystick-p4-cmd52-marker-positive-nc1-20260828\`

| Artifact | Bytes | SHA-256 |
|---|---:|---|
| `stage1-initial-boot.bin` | 16,449 | `7b72eb28b9573d5bdace2051965456c7fd27cda41358a19eba9a1aefabc7051f` |
| `stage1-armed-recovery.bin` | 16,456 | `c626598c3035f21ba1860892ee42ba0c343f81495e3e381ec6b876adf8d961b4` |
| `stage2-ssh-repro.json` | 1,096 | `271cdbfbc98c30fb75d319d4da2eea2bd551b40bdf6428e5b042757020774f12` |
| `stage2-marker-recovery.bin` | 15,886 | `d161fcb69457ad6f463982e65892a71d7bed94d86c3461fa49328f136b634ffe` |
| `stage3-post-recovery-control.bin` | 16,456 | `d241877c2e1bf490d75c8b2af0af9d6bdb750bc62bfdd5bce998b65010ae26ac` |
| `stage4-passive-uart-during-ssh.bin` | 0 | `e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855` |
| `stage4-passive-uart-during-ssh.bin.json` | 441 | `60d16855ed187b756e77e8a6cf97b8cde19feb75ed255a9f1b15a85947ca2634` |
| `stage4-passive-uart-ssh-repro.json` | 1,096 | `3b7019c86c3c0d2a44a5a354fa70fd43ea7ea06bec19e4bf026ada9d24830693` |
| `stage4-marker-recovery.bin` | 17,747 | `47dc398d14b6362e08eb0f410045eb9cb3bae9e99d0ce6a83255e54ff0597b7d` |
| `stage5-shot-arm-console.txt` | 676 | `a4b2db896968f3738b892d27c37ecee510298587b362335c9289fbe8cd57d0c1` |
| `stage5-passive-uart-during-gated-ssh.bin` | 0 | `e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855` |
| `stage5-gated-ssh-repro.json` | 1,094 | `76b2ca79233cd8eaabc387bab6a84d9e50ad5840caeb63fcaf373aa58cd9d68b` |
| `stage5-gated-marker-recovery.bin` | 17,143 | `dad58d0a06b21f89c0c178c3070ceb027692213def7859444276aff6530ce203` |

## Stage 1 — no SSH positive control

The first boot reached the expected functional state:

```text
M3-lab: wlan0 appeared
M3-lab: association complete; using static IPv4
          inet addr:10.255.10.161
M3-lab: starting password-enabled Dropbear on TCP/22
```

The marker line in the first boot capture was stale pre-reset content:

```text
easystick-boot: CMD52_MARKER magic=0x45534d34 armed=0x4ff16240 token_enter=0x00007b7c after_46=0x00007b7c before_47=0x00007e48 after_47=0x501081b8
```

It is not evidence for this shot. After waiting for Wi-Fi/Dropbear and
performing a controlled P4 reset without an SSH command, the next boot
reported:

```text
easystick-boot: CMD53_BB pa=0x50108080 reset_reason=11
easystick-boot: CMD52_MARKER magic=0x45534d30 armed=0x45534d31 token_enter=0x45534d32 after_46=0x45534d33 before_47=0x45534d34 after_47=0x45534d35
easystick-boot: CRASH_CAPSULE empty/invalid
```

Rootfs mount, Wi-Fi association, static IPv4, and Dropbear startup were also
present in this recovery capture. Kernel panic, runtime `Call Trace`, and a
crash capsule were not observed.

**Result: PASS for the marker infrastructure positive control.** `MAGIC` and
`ARMED` written by the Linux-side path were recovered after reset, and all
four stage words also contained their expected values.

This is not a predicate-free `ARMED`-only observation: normal Wi-Fi/ESP-Hosted
startup generated at least one matching CMD52 read before the reset, so the
four stage words were allowed to change. It does, however, prove that the
marker address, Linux store path, multi-word layout, and retention/recovery
path worked in this boot.

## Stage 2 — one SSH reproduction and recovery

The SSH event log recorded the same post-authentication wedge:

```text
TCP22_BEFORE reachable=True elapsed=0.375
SSH_AUTHENTICATED elapsed=1.063
CHANNEL_OPEN elapsed=1.078
EXEC_SENT command='id' elapsed=1.109
EXIT_TIMEOUT stdout_bytes=0 stderr_bytes=0 closed=False elapsed=13.125
TCP22_AFTER reachable=False error="TimeoutError('timed out')" elapsed=19.141
```

The SSH shot therefore **FAILED** the M3-lab command-acceptance criterion:
authentication, channel open, and command transmission succeeded, but no
stdout, stderr, exit status, or EOF arrived and TCP/22 became unreachable.

After the wedge, a controlled P4 reset recovered:

```text
easystick-boot: CMD53_BB pa=0x50108080 reset_reason=11
easystick-boot: CMD52_MARKER magic=0x00000000 armed=0x00000000 token_enter=0x00000000 after_46=0x00000000 before_47=0x00000000 after_47=0x00000000
easystick-boot: CRASH_CAPSULE empty/invalid
```

**Result: INCONCLUSIVE for the CMD52 fault boundary.** The all-zero recovery
does not prove that execution stopped before `TOKEN_ENTER`, `0x46`, or `0x47`.
It is compatible with:

- the exact pre-reset boot not reaching the arm or matching-read path;
- a reset/boot sequence clearing the marker before the observed recovery;
- the matching `reg=0x44,size=4` predicate not being entered;
- a Linux store not reaching the expected physical address in that run;
- stores executing but not surviving that run's reset; or
- the recovery read observing zero because of an address or initialization
  condition.

No UART was captured concurrently with the SSH wedge, so this shot cannot
exclude an uncontrolled reset before the requested controlled reset. The
`reset_reason=11` line belongs to the captured controlled-reset boot and does
not close that timing question.

The earlier `runtime2` console observation of a return after `0x46` remains
unproven by this non-console shot. The all-zero marker must not be converted
into an `M1-before` or `0x46` boundary claim.

## Stage 3 — post-recovery no-SSH control

To check whether the zero result represented a permanently broken marker path,
the boot produced by Stage 2 recovery was allowed to reach Wi-Fi/Dropbear and
was reset again without SSH. The next recovery reported:

```text
easystick-boot: CMD53_BB pa=0x50108080 reset_reason=11
easystick-boot: CMD52_MARKER magic=0x45534d30 armed=0x45534d31 token_enter=0x45534d32 after_46=0x45534d33 before_47=0x45534d34 after_47=0x45534d35
easystick-boot: CRASH_CAPSULE empty/invalid
```

Wi-Fi association, static IPv4, and Dropbear startup were present again.

**Result: PASS for a subsequent normal-boot re-arm and retention check.**
This reduces the likelihood of a globally wrong PA, broken six-word layout, or
permanently broken retention path. It does not explain why the preceding SSH
shot recovered zeros, and it does not provide a CMD52 boundary for that shot.

## Stage 4 — passive UART during SSH and repeat recovery

The candidate was then allowed to boot normally. A second SSH `id` shot was
started while `capture-boot.ps1` was running **without** `-Reset`, so the
collector forced RTS/DTR low and did not drive either control line. The passive
collector opened COM10 eleven times over its 45-second window and captured
zero bytes:

```text
stage4-passive-uart-during-ssh.bin: 0 bytes
port_opens 11 first_byte_seconds -1 control_lines_driven False
```

The paired SSH event log again showed:

```text
TCP22_BEFORE reachable=True elapsed=0.125
SSH_AUTHENTICATED elapsed=1.016
CHANNEL_OPEN elapsed=1.031
EXEC_SENT command='id' elapsed=1.047
EXIT_TIMEOUT stdout_bytes=0 stderr_bytes=0 closed=False elapsed=13.063
TCP22_AFTER reachable=False error="TimeoutError('timed out')" elapsed=19.063
```

After the controlled P4 reset, recovery reported the complete expected marker
set:

```text
rst:0x17 (CHIP_USB_UART_RESET),boot:0xc (SPI_FAST_FLASH_BOOT)
easystick-boot: CMD53_BB pa=0x50108080 reset_reason=11
easystick-boot: CMD52_MARKER magic=0x45534d30 armed=0x45534d31 token_enter=0x45534d32 after_46=0x45534d33 before_47=0x45534d34 after_47=0x45534d35
easystick-boot: CRASH_CAPSULE empty/invalid
```

**Result: PASS for the passive-reset observation and repeated marker
recovery; FAIL for SSH command acceptance.** No boot output was observed during
the passive window. This does **not** indicate that no reset occurred: the
window was 45 s, and the S0 shot later measured the r4 wedge's WDT reset at
`exec + 90.85 s`. The window closed ~44 s before a reset could fire, so its
zero-byte result carries no information about whether the board eventually
reset — see the Retraction in the Decision section below.

The repeated complete marker set also confirms that the Stage 2 all-zero
result is run-specific rather than a deterministic response to this SSH
wedge. It does not localize the wedge: normal Wi-Fi/ESP-Hosted startup writes
the four stage words before SSH, so a later complete set cannot be attributed
to the SSH transaction.

## Stage 5 — rejected `/dev/mem` shot gate

Before adding a new image, the ttyGS1 root shell was used to test whether
BusyBox `devmem` could clear the four stage words and write back
`MAGIC`/`ARMED` immediately before SSH. The console readback was:

```text
0x45534D30
0x45534D31
0x00000000
0x00000000
0x00000000
0x00000000
EASYSTICK_CMD52_SHOT_ARMED
```

The paired SSH shot reproduced the same wedge, and the passive UART collector
again captured zero bytes. After controlled reset, however, all six words were
zero:

```text
easystick-boot: CMD52_MARKER magic=0x00000000 armed=0x00000000 token_enter=0x00000000 after_46=0x00000000 before_47=0x00000000 after_47=0x00000000
easystick-boot: CRASH_CAPSULE empty/invalid
```

**Result: REJECTED as a shot-gate method.** The console readback proves only
that the `devmem` access path could observe its own writes; the subsequent
boot-shim read did not recover them. This is not evidence about the CMD52
boundary and the method is not used for the next shot. The clean gate must
call the kernel marker helper itself rather than rely on a userspace physical
mapping.

## Decision

The marker infrastructure gate is **PASS**. The SSH wedge remains
**REPRODUCED / FAIL**, while its CMD52 location remains **UNRESOLVED**.

**Retraction (2026-08-28).** An earlier version of this section claimed the
passive UART evidence "makes a visible automatic reset during the SSH wedge
unlikely." That claim is withdrawn: it was drawn from a window shorter than the
event it purported to rule out. Every passive window here was 45-48 s long
(Stage 4 ran 45 s; Stage 6 held SSH-start to collector-close for 46.8 s: SSH at
`09:37:44.464371`, close at `09:38:31.220594`). The later S0 shot, which held
the collector open for 260 s without resetting, measured the ROM banner at
`exec + 90.85 s` — the r4 image's 120 s MWDT fires ~91 s after the wedge
begins, because the last kernel watchdog feed precedes the exec by roughly the
~60 s feed period. A 45-48 s collector therefore closes about 44 s *before* the
reset can occur, so its zero-byte result is exactly what an eventual reset and
no reset both produce. Silence in a window shorter than the fault's time
constant is not evidence of absence. The Stage 2 all-zero marker value remains
unclassified for the separate reasons already listed, none of which this
retraction changes.

The six-word marker is proven as an infrastructure control, not
as an SSH-transaction discriminator, because normal startup can populate all
four CMD52 stage words before the SSH shot.

The next boundary experiment must therefore add an explicit shot gate or
per-shot generation after normal Wi-Fi startup, then instrument the lower
`0x46`/`0x47` call chain. It must preserve the existing six-word positive
control and use separate words for each lower boundary; the all-zero Stage 2
result remains unclassified. A kernel-side root-only shot-gate module
parameter is now being built; the rejected `/dev/mem` method is not part of
that candidate.

The prior `ES_CMD52 printk/pr_emerg` quiet-control contrast remains applicable:
`ssh-stacktrace-cmd52-quiet-control-uart-capture-2026-08-27.bin` (16,284 bytes,
SHA-256
`89c96cf29d299418f56548344e30b7742da0f164e835158ecafa993ac962696c`) had
`ES_CMD52=0`, no WDT marker, no panic, and no `Call Trace`, while its matching
`exec("id")` also timed out and TCP/22 became unreachable. That contrast rejects
the console observer as a necessary cause; it does not localize the fault.

## Stage 6 — kernel-side gated SSH shot with concurrent passive UART — 2026-08-28

The r4 NC=1 `m3-lab` candidate was flashed to P4 `COM10` only. The C6 image
was not erased or written, and the preserved stock readback remained
`229459f251eaf6222f0c07968702d72a3818e520da1788ce465027d969020c24`.
The post-flash boot reached Linux, `wlan0`, static IPv4
`10.255.10.161`, and password-enabled Dropbear.

The first attempt to pair the new gate with passive capture is retained as a
timing control, not as boundary evidence: its SSH started after the 45-second
passive-capture window had ended. The valid shot below was then run as one
continuous host-side sequence.
The discarded control files were `stage2-shot-arm-console.bin` (218 bytes,
SHA-256 `3fd643262d2920ed885f23307068e11fcbceb30f7d35d3b676af55497e752cba`),
`stage3-passive-ssh-shot.bin` (0 bytes, SHA-256
`e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855`),
`stage4-ssh-repro.json` (1,096 bytes, SHA-256
`5ddb11d18b5ad2c864339769f70b2bf637c725d050df3cbd875152442226883e`), and
`stage5-post-ssh-reset-marker.bin` (16,414 bytes, SHA-256
`8e825e6a112293cb79884d788c5a3de34ffd4ab37207febf6e4d9022efe6edd4`).
They are retained for audit only and do not support a boundary claim.

Follow-up artifacts are retained outside Git at:

`C:\Users\developer\tmp\easystick-p4-cmd52-marker-shot-r4-20260828\`

| Artifact | Bytes | SHA-256 |
|---|---:|---|
| `stage1-post-flash-boot.bin` | 16,455 | `2c222bec88c7a21fe649369974c1b3c6c238dee238b7c1d22f730061e116d949` |
| `stage6-shot-arm-console.bin` | 218 | `3fd643262d2920ed885f23307068e11fcbceb30f7d35d3b676af55497e752cba` |
| `stage6-shot-arm-console.bin.json` | 639 | `3d3b389a5855c3ec386c54e47dd63ddebe7eca1774589d08b92ce2b9f9ba942e` |
| `stage6-8-shot-orchestration.json` | 747 | `8ad0155c658d7338bfa79996ee45ea6d67fab314bcf521b4081fd935bf6428c8` |
| `stage7-passive-ssh-shot.bin` | 0 | `e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855` |
| `stage7-passive-ssh-shot.bin.json` | 441 | `7d1a93104873b3a1af5d8b7c04c540e9158a1a9d4e801e4b0d5d1dd91f0fe1ac` |
| `stage8-ssh-repro.json` | 1,096 | `95e33bf7cf40575a7019b457585dcea1025b2fd219bff722e5faa216eb15a75f` |
| `stage9-post-valid-shot-reset-marker.bin` | 16,426 | `df6cfe5bf0baaaaa67cbbee6d7fa6b9b4d4cef54b16fa6fd1e17875f8ed57cc7` |
| `stage9-post-valid-shot-reset-marker.bin.json` | 476 | `064f2e2affc9f9b38105eb1bf574bc5477197e564f5da42396dca1780524db7e` |

The gate used only the root-writable kernel parameter:

```text
printf 1 > /sys/module/esp32_sdio/parameters/cmd52_marker_shot
EASYSTICK_SHOT_GATE_READBACK=1
```

The gate metadata records `reset_requested=false` and
`control_lines_driven=false`. The command was sent at
`2026-08-28T09:37:40.974882+00:00`. The passive collector started at
`09:37:43.048834+00:00`, SSH started at `09:37:44.464371+00:00`, and the
collector remained open through `09:38:31.220594+00:00`. It opened COM10
eleven times, drove no reset control lines, and captured zero bytes.

The single SSH shot again recorded:

```text
TCP22_BEFORE reachable=True elapsed=0.078
SSH_AUTHENTICATED elapsed=0.687
CHANNEL_OPEN elapsed=0.703
EXEC_SENT command='id' elapsed=0.734
EXIT_TIMEOUT stdout_bytes=0 stderr_bytes=0 closed=False elapsed=12.734
TCP22_AFTER reachable=False error="TimeoutError('timed out')" elapsed=18.734
```

**Result: FAIL for SSH command acceptance.** No stdout, stderr, exit status,
or EOF arrived; TCP/22 became unreachable.

After the wedge, the P4-only `capture-boot.ps1 -Reset` recovery reported:

```text
easystick-boot: CMD53_BB pa=0x50108080 reset_reason=11
easystick-boot: CMD52_MARKER magic=0x45534d30 armed=0x45534d31 token_enter=0x45534d32 after_46=0x45534d33 before_47=0x45534d34 after_47=0x45534d35
easystick-boot: CRASH_CAPSULE empty/invalid
```

The reset capture requested the P4-only HardReset-compatible RTS/DTR sequence,
captured 16,426 bytes, and received its first byte after 2.105 seconds. No
`Call Trace` or panic text was present in the recovery capture.

The same recovery capture also carried the retention black box's own CMD53
record, which this document previously quoted around but did not show
(`stage9-post-valid-shot-reset-marker.bin`, 16,426 bytes, SHA-256
`df6cfe5bf0baaaaa67cbbee6d7fa6b9b4d4cef54b16fa6fd1e17875f8ed57cc7`):

```text
easystick-boot: CMD53_BB VALID gen=89176 seq=15 cmd_arg=0x97ec0000 req=1 cmd_done=1 cmd_err=0 idmac=1 data_over=1 data_err=0 cto=0 dto=0 end=1 bb2=1 bb3=1 bb4=1 bb5=1 bb6=1 bb4_ret=0 bb5_ret=0 bb6_ret=0
easystick-boot: CMD53_BB boundaries dma_in=1 dma_out=1 bh_in=1 data_in=1 data_out=1 irq_out=1
easystick-boot: CMD53_BB request_end enter=1 before_next=0 after_next=0 idle=1
easystick-boot: CMD53_BB data_st=0x50108094 pending_data=0x00000008
easystick-boot: CMD53_BB idsts=0x00000101
easystick-boot: CMD53_BB cmd_e=0 data_e=0 xfer=512
```

This line is evidence that the SDIO/MMC transmit path was healthy at the
recorded wedge, not merely the CMD52 read: the last CMD53 write completed all
fifteen stages (`seq=15`, every `bb2..bb6=1`) with `end=1`, no timeout
(`cto=0 dto=0`), no error (`cmd_err=0 data_err=0`), and the request-end
bookkeeping reads `idle=1`. `before_next=0 after_next=0` must not be read as a
stop boundary: patch 0053 only marks those in the queue-non-empty branch, so
their absence means the queue was empty, i.e. this was the last request, not a
stalled one. This strengthens the "localized past `sdio_readb(0x47)`"
conclusion below — the CMD52 read chain completed and the CMD53 write path was
clean, so the stall lies above the SDIO transfer layer.

**Result: PASS for the gated marker shot and retention path.** All six words
survived the controlled reset. For a matching `reg=0x44,size=4`
`TOKEN_RDATA` read after the kernel-side gate, the evidence reaches
`TOKEN_ENTER`, the return from `sdio_readb(0x46)`, the point immediately before
`sdio_readb(0x47)`, and the point immediately after `sdio_readb(0x47)`.
This excludes a stop before or inside either of those two `sdio_readb` calls
for the observed matching transaction.

The marker stores are fixed constants and do not carry a generation or
first-hit owner. Therefore this shot proves a complete matching CMD52 read
sequence after the gate, with strong temporal correlation to the one SSH
attempt, but it does not prove that SSH was the only operation that could have
triggered it or that the wedge was caused by the read. It also does not prove
that `esp_read_reg()` returned successfully, that `mmc_request_done()` ran, or
that the later network/SSH response path completed.

## Updated decision

- Kernel-side sysfs shot gate: **PASS**; readback was `1`, with no `/dev/mem`
  use.
- Concurrent passive UART control: **PASS** as a non-reset observer; zero
  bytes and no driven control lines were recorded.
- SSH `exec("id")` acceptance: **FAIL / REPRODUCED**.
- CMD52 boundary: **localized past `sdio_readb(0x47)` for the observed
  matching transaction**, but the exact post-read / MMC completion boundary and
  causal mechanism remain **UNRESOLVED**.
- C6: **unchanged**; no erase or write operation was performed.
