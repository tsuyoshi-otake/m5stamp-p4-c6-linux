# S0b re-shot: a WDT reset captured inside the same window as the beacon

Date: 2026-08-29. Board: EasyStick Stamp-P4, Rev0.15 (M1), r4 image,
`vendor/linux` at `acb7cf4c1184`. DUT: `10.255.10.161`. Host: COM10
(USB-Serial/JTAG, VID:PID `303A:1001`).

## Why this shot exists

`ssh-wedge-s0b-console-beacon-2026-08-28.md` ("the first S0b report") fired
its SSH trigger against a DUT whose ICMP/TCP were already dead at
`elapsed=0.0`, before the trigger. That shot's beacon evidence therefore
could not settle whether the documented wedge is:

- **(A) global stall** — interrupts off or the scheduler dead; nothing runs.
- **(N) confined stall** — the scheduler/timer/console stay alive, but the
  SDIO network TX path and the watchdog-feed work item are blocked.

Per that report's "Recommended next steps" item 2, `s0b-run.py` was extended
with a preflight ICMP-reachability gate (`preflight_wait_reachable()`, reusing
`s1-liveness.py`'s verified `run_ping()` rather than re-implementing ping
parsing — CLAUDE.md Section 14.13) that blocks the SSH trigger until a
genuine, text-parsed ICMP reply is observed, up to 10 retries at 1 s spacing,
and records every attempt. The DUT had also gone silent for ~14.5 h between
the first shot and this one (documented and resolved in that report as a
physical power loss, fixed by the user unplugging/replugging USB); a fresh
console beacon (`ESLIVE`, PID 138) was re-armed after the resulting reboot and
confirmed ticking before this shot.

## Artifacts (evidence root: `s0b-console-beacon-20260829/`, kept outside Git)

| Bytes | SHA-256 | File |
|---:|---|---|
| 8040 | `2081f55394db70c2482c427029e8074d53336fc11660f260e3b669a1d3556d93` | s0b-console-send.py |
| 6413 | `4fc3ae017ca59111577af57eda6051629e86be21dd206bad53204818602e32e0` | s0b-run.py |
| 8298 | `6d7cb990501f84bb5615773f3f4ae2aeef1af308fc6278aec5d7aeba6e972950` | s0b-analyze.py |
| 1049 | `943063d4246dfe52d316877fddf254e654704542650a5d2cb7d6ab579463d294` | s0b-preflight-beacon-check-20260829.json |
| 16679 | `0b118c7b317a80174daff34a86cfbb41a4cea253ffe56061c6abcba155f3a75a` | s0b-passive-console-and-wdt-window.bin |
| 341 | `2e7e6277298d535b6dbc59abe4e614dbf5359af48b8b0fee4b9f3665b5f17192` | s0b-passive-console-and-wdt-window.bin.json |
| ~155000 | `1cbcee129ebe864acb11757c02f01be4e9f22539ebd46130af65dfd4d448ac6a` | s0b-icmp-liveness.jsonl |
| ~45000 | `d07981c303c3e9e801a50bb59324b48081b1dd7486aa7ef5bf568d929ed733fb` | s0b-port-presence.jsonl |
| — | `a32d1587414af3f0aa4644447734837e32162cc77d8b4dbb14bf81d192a52971` | s0b-ssh-repro.json |
| — | `d2b9ac3297821f4e8b9f4803e99b70efa26f43c971d77d82f5c7eb86762c07db` | s0b-orchestration.json |
| 268 | `4b4ad5585e9c02ae9d6808d7854144fb79fa9e58d0536f6026084bd593f78cd4` | s0b-run.stdout.txt |
| 2262 | `8775f97207d4f2f415e0557cce08f3ca575b5a7c868ecd2ee8cabed7a51857bd` | s0b-analysis.json |

`s0b-analyze.py` was extended this shot (still under CLAUDE.md Section
14.14/14.13 discipline — nothing below is hand-arithmetic) to detect a ROM
boot banner and WDT reset-reason line inside the capture, to detect a
`CRASH_CAPSULE` status line, to prove line-ordering between the last beacon
tick and the reset banner, and to correlate every event in `s0b-ssh-repro.json`
(host-clock, measured) against the beacon's own extrapolated last-tick time
into one merged, sorted timeline. `s0b-run.py` was extended with the
preflight gate described above; both changes are recorded in this evidence
directory as the exact script versions used for this shot, distinct from the
versions checksummed in the first S0b report.

## Timeline

All times UTC, 2026-08-29.

| Time | Event |
|---|---|
| 02:22:46.233 | Passive UART capture opens COM10 (sole reader). |
| 02:22:46.245 / .255 | ICMP sampler and port-presence sampler start. |
| 02:22:50.256–.413 | Preflight gate: attempt 1 gets a parsed, text-matched ICMP reply (`matched_by: "ttl"`) — **REACHABLE**, no retry needed. |
| 02:22:50.413 | SSH trigger starts (`ssh_started_utc`). |
| 02:22:50.475 | `TCP22_BEFORE`: reachable. |
| 02:22:51.006 | `SSH_AUTHENTICATED`. |
| 02:22:51.022 | `CHANNEL_OPEN`. |
| 02:22:51.069 | `EXEC_SENT` — `id` sent over the open channel. |
| ~02:22:52.07 (icmp elapsed 6.0s) | ICMP sampler's `icmp_reply` flips `True → False` — **~1.0 s after `EXEC_SENT`**, the same ballpark as S0's own positive-control figure of 1.56 s. |
| **02:23:01.565 (estimated)** | **Last `ESLIVE` beacon tick** (uptime `756.74`), extrapolated from the beacon's own measured 1.0246 s cadence anchored at the capture's first byte. |
| 02:23:03.069 | `EXIT_TIMEOUT`: the SSH probe's own 12 s exec-completion wait expires with 0 stdout/stderr bytes and the channel still open — the exec never returned. |
| *(immediately next non-blank line after the last beacon tick, zero lines between)* | `ESP-ROM:esp32p4-eco2-20240710` boot banner, then `rst:0x7 (HP_SYS_HP_WDT_RESET)`. |
| 02:23:09.085 | `TCP22_AFTER`: `reachable: false`, `TimeoutError('timed out')`. |
| ~02:22:52 – 02:24:31 (icmp elapsed 6.0–105.06 s) | ICMP unreachable continuously for **99.06 s**. |
| ~02:24:31 (icmp elapsed 105.06 s) | ICMP sampler's `icmp_reply` flips back `False → True`. |
| through 02:26:16 (icmp elapsed 209.7 s, window close) | ICMP reachable (one single-sample blip at elapsed 190.7 s, back up the next sample). |
| whole window | `port-presence` (Windows PnP `present` flag): `True` at all 210 samples — no PnP-visible disconnect. |

## Result: this shot exercised the documented wedge

Unlike the first S0b shot, the preflight gate confirmed the DUT was reachable
(`preflight_verdict: "REACHABLE"`, first attempt) immediately before the SSH
trigger fired, and the SSH probe's own event trace matches the wedge
signature stated in `ssh-stacktrace-cmd52-positive-control-2026-08-28.md`
(S0): a completed, authenticated exec whose reply never returns, followed by
network death within a few seconds and a hard reset — here proven by a
captured ROM/WDT banner rather than inferred from a later reconnect.

From `s0b-analysis.json`:

- `verdict: "RESET_DURING_CAPTURE"`.
- `reset_detected_in_capture: true`, `reset_reason: "HP_SYS_HP_WDT_RESET"`.
- `beacon_silent_immediately_before_reset: true` —
  `lines_between_last_beacon_tick_and_reset: []`. The ROM banner is the very
  next non-blank line in the byte stream after `ESLIVE 756.74`. Nothing else
  — no panic dump, no other diagnostic line — was produced by the kernel in
  between.
- `crash_capsule_state: "empty/invalid"` — the `easystick-boot: CRASH_CAPSULE
  empty/invalid` line from the current boot-shim (see CLAUDE.md Section
  14.23) reports no captured crash context for this reset, despite the
  ordering fix that made `easystick_capture_crash()` run before `pr_emerg`.
- The merged `event_order` places `last_beacon_tick` (extrapolated,
  `02:23:01.565Z`) strictly between `ssh:EXEC_SENT` (`02:22:51.069Z`,
  measured) and `ssh:EXIT_TIMEOUT` (`02:23:03.069Z`, measured): the beacon
  went silent while the SSH probe's own exec-completion wait was still
  pending, roughly 1.5 s before that wait itself timed out.

## Interpretation

**This result favors (A), a global stall, over (N), a confined SDIO/watchdog
stall, for this shot.** The beacon exercises none of the paths S0/S0b's own
hypothesis assigns to (N): it is a userspace shell reading `/proc/uptime` and
writing to the USB-Serial-JTAG console, which does not touch the SDIO
controller, `wlan0`, or the watchdog-feed work item. For it to stop ticking,
either the scheduler stopped running that shell process, or the console TX
path itself became permanently blocked with no completion interrupt —
either way, something broader than "SDIO TX and the watchdog-feed worker
alone" stopped running, and it stopped before the SSH probe itself had even
concluded the exec was stuck. This is the outcome the first report's own
purpose section named as **(A)**'s signature: "ESLIVE stops at the exec and
resumes only after the ROM banner."

This does not fully exclude every (N)-shaped explanation: if the console UART
driver's TX-complete path specifically shares an interrupt, lock, or work
queue with the SDIO TX path, a stall confined to that shared resource could
present identically. The evidence here proves the beacon and normal network
traffic stopped together and stayed stopped until the WDT fired; it does not
by itself prove *which* shared kernel resource is the actual root cause.

**Two new, unexplained observations, deliberately not theorized further
here:**

1. **The crash capsule is still empty.** CLAUDE.md Section 14.23 fixed the
   pretimeout-handler ordering specifically so a hard lock could be
   distinguished from a printk/console deadlock by an empty-vs-populated
   capsule. This reset produced an empty capsule anyway. Candidate
   explanations — the pretimeout IRQ itself never fired before the hard
   reset, the capture path inside it also stalled, the grace window is still
   too short for this failure mode, or the capsule write path depends on
   something this stall also blocks — are not distinguished by anything
   captured in this shot.
2. **ICMP took ~90 s longer to recover than the boot log suggests it should
   have.** The captured boot log runs through Wi-Fi association, static IPv4
   assignment, and Dropbear startup, ending at a live `~ #` prompt — all of
   which appear complete well inside the first ~20 s of the window. Yet the
   independent ICMP sampler shows continuous unreachability for 99.06 s
   (elapsed 6.0–105.06 s) before recovering. No beacon was re-armed after
   this reset and nothing else was printed to the console in that gap (301
   total lines, all accounted for by the beacon-tick-then-boot-log sequence
   above), so nothing captured here explains the delay — it is flagged, not
   diagnosed.

## What this evidence cannot see

1. A sub-tick (<~1.02 s) stall — the beacon samples once a second.
2. The reset's exact instant. The capture has no per-line timestamp; only
   ORDERING is proven (the ROM banner is the next non-blank line after the
   last beacon tick, with nothing in between), not a timestamp for the reset
   itself. The `last_beacon_tick_estimated_utc` value is an extrapolation
   from the beacon's own measured cadence, not a second independent
   timestamp — see `s0b-analysis.json`'s own `last_beacon_tick_anchor_method`
   field.
3. Anything about the ~190 s of console silence after the fresh boot's `~ #`
   prompt (no beacon was armed on this new boot, so this shot cannot say
   whether the kernel was live, idle, or stalled again during the ICMP
   recovery gap described above).
4. Why the crash capsule was empty for this specific reset.
5. Whether the console UART TX path shares a lock, interrupt, or work queue
   with the SDIO TX path — the one gap that would let a genuinely confined
   (N) stall present with this shot's exact signature.

## Recommended next steps

1. Re-arm the beacon immediately after every future reset (this shot did
   not), so a repeat shot's console silence during the post-reset ICMP-outage
   window is itself instrumented instead of unobserved.
2. ~~Investigate the crash-capsule miss directly~~ — root-caused without
   needing a rebuild; see Addendum below. **Rebuild, reflash, and re-run S0b
   is still required to confirm the fix in vivo** (the build container was
   down at analysis time — `docker info` failed to reach
   `npipe:////./pipe/docker_engine`).
3. Treat the ~90 s ICMP recovery delay after a fresh boot as its own tracked
   observation, separate from the SSH/exec wedge, until a shot instruments
   the Wi-Fi association/`cfg80211`/`esp-hosted-ng` path across that specific
   window.
4. If a kernel-side investigation of interrupt/lock sharing between the
   console UART driver and the SDIO TX path is undertaken, use this shot's
   precise ordering (beacon silent strictly between `EXEC_SENT` and the
   probe's own `EXIT_TIMEOUT`) as the target window to instrument.

## Addendum (same day): the empty capsule root-caused, independent of ordering

`assert_pretimeout_order.py` (Section 14.23's checker) proved the *software*
capsule-before-`pr_emerg` ordering was correct in the build that produced this
shot, yet the capsule was still empty. Re-reading `esp32p4_wdt_arm()` in
`kernel-patches/0054-easystick-wdt-crash-capsule.patch` against every real
`wdt_hal_config_stage()` call site in the vendored `esp-idf` tree (`int_wdt.c`,
`task_wdt_impl_timergroup.c`, and every SoC's `rtc_wdt`/`clk.c` reset path —
esp32, c2, c3, c5, c6, c61, h2, h21, h4, p4, s2, s3) found a second, wholly
independent bug: every one of those call sites configures a later MWDT stage's
register as an **absolute** tick count from the last feed — stage 1 is always
programmed as `2 * stage0_period`, never `1 * stage0_period` — because all
stages compare against one shared free-running counter
(`components/hal/esp32p4/include/hal/mwdt_ll.h`'s `wdt_stg0_hold` /
`wdt_stg1_hold`, backed by `TIMG_WDTCONFIG2`/`TIMG_WDTCONFIG3`, the exact same
peripheral and register offsets this Linux driver pokes directly).

The patched `esp32p4_wdt_arm()` instead wrote:

```c
writel(pretimeout_ticks, wdt->base + TIMG_WDTCONFIG2);  /* stage 0: correct */
writel(grace_ticks,      wdt->base + TIMG_WDTCONFIG3);  /* stage 1: WRONG   */
```

`grace_ticks = min(30000, timeout_ticks/2)` is always `<= timeout_ticks/2 <=
pretimeout_ticks`, so stage 1's threshold was **smaller** than stage 0's. On
hardware where every stage compares against the same counter, this means the
hardware reset (stage 1) can fire before the counter ever reaches stage 0's
threshold — the pretimeout interrupt, and therefore
`easystick_capture_crash()`, may never run at all, independent of whether the
CPU is otherwise perfectly healthy. This reproduces the exact observed
symptom (empty/invalid capsule on a genuine WDT reset) through a path the
ordering fix cannot touch: a register-value error, not a runtime IRQ-masking
state.

**Fix applied** (2026-08-29, same day, this repository):
`kernel-patches/0054-easystick-wdt-crash-capsule.patch` now writes
`timeout_ticks` (the full, original absolute timeout — the natural
`pretimeout_ticks + grace_ticks`) into `TIMG_WDTCONFIG3`, restoring stage 1's
threshold to fire `grace_ticks` ticks after stage 0's interrupt, as intended.

**Checker extended, not just the source**: per CLAUDE.md Section 14.2 ("a
checker only proves what it looks at"), `assert_pretimeout_order.py` gained
check `C8`, asserting the exact expression written to `TIMG_WDTCONFIG3` is
`timeout_ticks`. Negative-tested against both a synthetic mutation and the
real pre-fix patch text (substituting `grace_ticks` back in) — both fail C8
with the fixed patch passing. The module docstring's own "what this cannot
see" item 3 already predicted the *symptom* ("no source-level check can see"
an IRQ never taken) but attributed it only to a runtime IRQ-off hard lock;
this bug produces the identical symptom from a source-visible register value
instead, which is why it is now checked at the source level rather than left
as an accepted blind spot.

**Still outstanding**: this fix has not yet been built or flashed (the
Rancher Desktop / Docker build container was down — `docker info` returned
"failed to connect to the docker API"). The empty capsule observed in this
shot's evidence is fully explained by this bug, but that explanation is not
yet confirmed by a fresh in-vivo capture showing a populated capsule after
the fix. This does **not** change the (A)-favoring interpretation above: the
beacon-silence-then-reset evidence is independent of the capsule question
entirely.

---

## Addendum 2026-08-29 (later same day): in-vivo confirmation, fix closes the gap

The fix was built (Docker container `p4-wdt-fix-build`, evidence archived
outside Git at `C:\Users\developer\tmp\p4-wdt-capsule-fix-20260829\`), its own
fail-closed build gate (`wdt_crash_fail_closed_after_linux()` in
`build-m1.sh`) passed and recorded `wdt-crash-capsule-attestation.json`
confirming the compiled `esp32p4_wdt.c` contains
`writel(timeout_ticks, wdt->base + TIMG_WDTCONFIG3)`, and the image was
flashed to the physical DUT via the reviewed `flash-candidate.ps1
-AllowCandidateWrite` gate (all 6 partitions verified byte-for-byte after
write).

A positive control was then run against the real hardware over the UART
console (this build has no Wi-Fi provisioned, so the original SSH-wedge
network path could not be exercised on this image; instead the WDT driver's
own `test_no_feed` module parameter was used to force a genuine no-feed
condition): `echo 1 > /sys/module/esp32p4_wdt/parameters/test_no_feed` was
issued at DUT uptime ~00:04:48. The full sequence captured on the console,
uninterrupted, in one continuous serial session
(`C:\Users\developer\tmp\p4-wdt-capsule-fix-20260829\wdt-positive-control-long-2026-08-29.log`):

```
EASYSTICK_WDT CAPSULE_COMMIT seq=3983531461 reason=1
EASYSTICK_WDT PRETIMEOUT
ESP-ROM:esp32p4-eco2-20240710
...
rst:0x7 (HP_SYS_HP_WDT_RESET),boot:0xc (SPI_FAST_FLASH_BOOT)
...
W (45) boot.esp32p4: CPU has been reset by WDT.
...
easystick-boot: CRASH_CAPSULE VALID seq=3983531461 reason=1 captured_cpu=0 epc=0x483c8d78 ra=0x483c8d74 sp=0x48527ef0 gp=0x485af4d4 tp=0x48529480 status=0x00011880 cause=0xb8000015 badaddr=0x00000000 wdt0=0xb81f8000 wdt1=0x9c400000 wdt2=0x00015f90 int=0x00000004 stack_len=32
easystick-boot: CRASH_CAPSULE FLASH_OK slot=5
```

This is a genuine hardware WDT reset (`HP_SYS_HP_WDT_RESET`, independently
confirmed by the boot-shim's own `CPU has been reset by WDT.` line), and the
**boot-shim** — the only component that actually reads the retention capsule
back, per `cmd53-bb/README.md` — reports it `VALID` on the very next boot,
with `seq=3983531461` matching the `CAPSULE_COMMIT` line exactly and
`reason=1` correctly identifying WDT as the cause. This is the fix closing
the exact gap this report's root-cause section describes: before the fix,
stage 1 (hardware reset) could fire before stage 0 (pretimeout interrupt +
capsule capture) ever ran; here, `PRETIMEOUT` and `CAPSULE_COMMIT` are both
observed before the reset, and the capsule survives to be read back
populated rather than empty.

Evidence: `wdt-positive-control-long-2026-08-29.log` (full console capture),
`wdt-crash-capsule-attestation.json` (build-time proof of the source fix),
`SHA256SUMS.txt` (checksums tying build artifacts to what was flashed) — all
under `C:\Users\developer\tmp\p4-wdt-capsule-fix-20260829\`.

This closes the "still outstanding" item above. The original SSH-wedge
reset-capsule question (whether *that* specific hard-reset event, on the
network-provisioned build, would now populate its capsule) remains
unconfirmed on that exact build/scenario, but the underlying mechanism the
bug affected — pretimeout-before-reset ordering and correct capsule capture
on a genuine WDT reset — is now verified end-to-end on real hardware.

## Addendum 2026-09-02: ICMP return is post-`rst:0x7` association

The ~90 s ICMP gap is closed as an open "revival" mystery. Measured times
from `s0b-orchestration.json`, `s0b-analysis.json`, and
`s0b-icmp-liveness.jsonl`:

- `EXEC_SENT` 02:22:51.069Z.
- ICMP last-true → first-false at elapsed 6.0 s (02:22:52.755Z), ~1.7 s after exec.
- ROM / `rst:0x7 (HP_SYS_HP_WDT_RESET)` immediately after last `ESLIVE`
  (estimated 02:23:01.565Z).
- ICMP first-true after that gap: elapsed 105.063 s = 02:24:31.808Z.

rst → ICMP ≈ 90.2 s. ARP is `absent` through the gap and `present` again
on the first returning ping. That is a new kernel after WDT reset, then
Wi-Fi association — not ICMP returning on the wedged kernel.

Do not equate this 90 s with the later 0054 epoch shift (delay110
reset+184.5 → reset+274.5). Same digit, different interval.

This S0b Image still had the pre-fix 0054 `CONFIG3=grace_ticks` programming
(same-day addendum above). exec→rst here was ~10.5 s on a 743–756 s
uptime leftover, not the current delay110 73.5 / 163.7 s windows.
