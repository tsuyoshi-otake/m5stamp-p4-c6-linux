# S0b: console-beacon separation of a global stall from a confined stall

Date: 2026-08-28 (shot), analyzed 2026-08-29. Board: EasyStick Stamp-P4,
Rev0.15 (M1), r4 image, `vendor/linux` at `acb7cf4c1184`. DUT: `10.255.10.161`.
Host: COM10 (USB-Serial/JTAG, VID:PID `303A:1001`).

## Purpose

S0 (`ssh-stacktrace-cmd52-positive-control-2026-08-28.md`) reproduced the
documented SSH wedge and proved it is not confined to `dropbear`: ICMP dies
1.56 s after a post-auth `exec("id")`, the kernel watchdog stops being fed,
and the board hard-resets ~90.85 s later. Every S0 channel travelled over
Wi-Fi, so S0 could not distinguish:

- **(A) global stall** — interrupts off or the scheduler dead; nothing on the
  DUT runs.
- **(N) confined stall** — the scheduler and timer interrupt are alive, but
  the SDIO network TX path is blocked (so ICMP cannot leave) and the
  watchdog-feed work item is blocked (so the WDT still fires).

S0b armed a 1 Hz beacon on the DUT's **serial console** before the shot,
`(while :; do read u _ < /proc/uptime; echo ESLIVE $u; sleep 1; done) &`,
which does not cross Wi-Fi and depends on the timer interrupt, the
scheduler, and the console TX path. It then held a passive UART capture
open across the SSH trigger, without ever driving DTR/RTS (the ESP32
USB-Serial/JTAG reset strap) and without requesting a reset.

## Artifacts (evidence root: `s0b-console-beacon-20260828/`, kept outside Git)

| Bytes | SHA-256 | File |
|---:|---|---|
| 581 | `2056fd226cbc9dd6a94c7a2565f1cc761b9bbc18377dcc5fafd6bfd15288e68f` | s0b-00-probe.json |
| 920 | `ea3856d3f2b80ccd3d789b414c6d1c36732ce18962edd6c0e96adc5fbd7abe57` | s0b-01-arm-beacon.json |
| 528 | `06fe95dec52a40816a66ee9085b36f244cb92a0a4ed00bedd42c4757d67b8563` | s0b-02-live-network-probe.json |
| 8040 | `2081f55394db70c2482c427029e8074d53336fc11660f260e3b669a1d3556d93` | s0b-console-send.py |
| 141208 | `778922a84c96a2d3fa645cc139bcc4856ec79ff062151dc830f6adba9b36e516` | s0b-icmp-liveness.jsonl |
| 1216 | `3947d80088847c326146d6a8863cb5152ac08866aefde5293cd2e34c7b1b0438` | s0b-orchestration.json |
| 3618 | `fc110cc296baac9f25353f91d1f9fba7b28052db648ea39ebc2fa2a403794c12` | s0b-passive-console-and-wdt-window.bin |
| 480 | `270d39174d75e7ef6481b917831f1c473ea4bb3d371622e0a1066bdac67d4468` | s0b-passive-console-and-wdt-window.bin.json |
| 45024 | `c0e9eaec4a113c54d4c5c7b0ef4db6160cbdaf78e83a679e958bc3e9075a75c0` | s0b-port-presence.jsonl |
| 5627 | `a9f98ee8eded2ed1ea5682676489fba5141d47b00880f655592ca577ab61368d` | s0b-run.py |
| 458 | `3830d1daa47bbde9879ce316329a8b2f11ae1ace03b1adf4e9d99ef705aebcd0` | s0b-run.stdout.txt |
| 5574 | `c40376f262ced8cc5cc091e840f723e3127510374793d329d6efff8c6e937ac0` | s0b-analyze.py |
| 954 | `f874a422e424b66af016b83a0ca4965ab7c4ce2517213b6bd691ae8ff9f6aa29` | s0b-analysis.json |

`s0b-analyze.py` computes every number in the Result section below from the
raw capture; nothing here is hand-arithmetic (CLAUDE.md Section 14.14/14.13).

## Timeline

| UTC | Event |
|---|---|
| 11:08:38–11:08:42 | Console probe: shell prompt live, `echo ESPROBE_OK` round-trips. Network not exercised by this step. |
| 11:09:00–11:09:09 | Beacon armed (PID 150); read back `ESLIVE 2829.02` … `2839.26` across a close/reopen of the port. |
| 11:14:00.048 | S0b passive capture opens COM10 (sole reader). |
| 11:14:00.063 / .078 | ICMP sampler (`s1-liveness.py`, reply parsed from text, never from ping's exit code) and port-presence sampler start. |
| 11:14:00.0–11:17:36 | **ICMP `reply` is `None` (no reply) for all 210 one-second samples, from `elapsed=0.0` onward** — i.e. before the SSH trigger below ever fired. |
| 11:14:04.093–11:14:22.638 | SSH trigger attempt: `socket.connect()` itself times out (`TimeoutError('timed out')`) — the TCP handshake to port 22 never completes. This is **not** the documented wedge signature (a completed exec whose reply never returns); the network path was already unreachable before the trigger. |
| 11:14:00–11:17:36 | Port-presence: `present=True` at every one of 210 samples, no re-enumeration → no USB reset occurred. |
| 11:17:33–11:17:36 | Passive capture closes; last beacon line `ESLIVE 3338.98`. |

## Result: console beacon evidence

From `s0b-analysis.json`:

- 226 lines captured, all recognized as `ESLIVE <uptime>`, zero unparsed lines.
- `first_uptime_s = 3108.58`, `last_uptime_s = 3338.98`, span `230.4 s`.
- Inter-tick delta: **min 1.02 s, max 1.03 s, mean 1.024 s** — no delta ever
  exceeds twice the mean; `any_backward_step = false`.
- Verdict: `CONTINUOUS_NO_RESET`.
- Mapping the SSH connect-attempt window (11:14:04.093Z–11:14:22.638Z) onto
  the capture (anchor: `passive_started_utc + first_byte_seconds`, spacing
  from the sequence's own measured mean period) brackets **uptime 3110.62 to
  3129.06**, fully inside the capture.

Across that exact bracket — the entire span during which the SSH `connect()`
call was blocked and eventually timed out — the beacon never missed a tick.
The kernel's timer interrupt, scheduler, a userspace shell, and the console
TX path were all demonstrably alive and unstalled to within the beacon's own
resolution (~1.02 s). No hard reset occurred anywhere in the 210 s window,
which extends past the 120 s DTS stage-1 WDT timeout measured from any
plausible starting point inside the capture.

## Interpretation

**(A), a global stall, is ruled out for this window.** Interrupts were not
off and the scheduler was not dead at any point the capture covers.

**This shot did not exercise the documented SSH wedge as designed.** The
premise of S0b was to fire the trigger against a network-healthy DUT and
watch the wedge develop. Instead, ICMP (and, per the SSH traceback, TCP)
were already unreachable at `elapsed=0.0`, roughly 5 minutes after the
beacon-arming console session proved the console alive — network death
happened silently in that gap, with no operator action, and did **not**
crash the kernel (proven by the console beacon for the following 230 s of
uptime). The SSH failure recorded here is a `connect()` timeout, not the
documented "connects, execs, exec never returns" signature, and no watchdog
reset ever fired in this window — which itself does not match the
documented wedge's ~90.85 s-to-reset signature. This event is therefore
evidence of **a network-reachability failure that leaves the kernel and
console fully healthy**, but it is **not established to be the same fault**
as the SSH/exec wedge S0 characterized: attribution to a common root cause
is open, not concluded.

**New, unplanned, currently unexplained state (2026-08-29T01:47:50Z, ≈14h33m
after this capture closed):** a fresh passive listen and an active
`s0b-console-send.py` probe (`s0b-02-live-network-probe.json`) both got
**zero bytes** from COM10 — no beacon output, no local echo of a sent
newline, no response to `cat /proc/uptime; ip -br addr show; ip route; dmesg
| tail`, across roughly 11 s of listening with control lines never driven.
`ping` to the DUT fails (`Destination host unreachable` from the local
router, i.e. no ARP entry). Windows still reports COM10 present and
`Status: OK` for the same VID/PID, and no leftover process on this host held
the port between the S0b shot and this check (`tasklist` for `python.exe`
and `powershell.exe` returned empty), so nothing on this machine touched the
DUT in the intervening ~14.5 hours. Because the armed beacon (PID 150) is an
unconditional infinite loop, its total silence now — after it ticked
perfectly through the entire S0b window — means the DUT's state has changed
since 11:17:36Z: either the beacon's shell session ended, or the kernel
itself is no longer producing console output, or the board lost power. This
was first observed after a long, unwitnessed gap in this session and **is
not attributable to anything done here**; it is flagged for physical
inspection (power/LED state) rather than diagnosed further remotely, per the
project's caution against unattended hardware manipulation.

**Resolved 2026-08-29 (same day, later).** A physical USB unplug/replug produced
a complete, fresh boot log on COM10 (RISC-V kernel init through `dw_mmc`/SDIO
card enumeration, `wlan0` appeared, `wpa_supplicant` association, static IPv4
`10.255.10.161`, Dropbear start, a live `~ #` prompt) and ICMP replies
resumed (4/4, 93-130 ms). A full boot sequence appearing only after a physical
unplug, on a USB-bus-powered board, is consistent with **the DUT having lost
power outright**, not with a kernel hung-but-not-reset: a live kernel does not
replay its own boot log on a bare USB replug. The cause of the power loss
itself (cable, host port, host sleep) remains unknown and is not established
to be related to the SSH wedge under study.

## What this evidence cannot see

1. A sub-tick (<~1.02 s) stall — the beacon samples once a second.
2. Any relative drift between the DUT's monotonic clock and the host wall
   clock; the host-UTC-to-uptime mapping is anchored once and extrapolated,
   not re-anchored against a second independent event.
3. Why the network died before the trigger fired, or what state the DUT was
   actually in during the ~5-minute unobserved gap between the beacon-arm
   console session and this capture's start.
4. Whether arming the beacon itself perturbs the fault being studied (stated
   already in `s0b-console-send.py`'s own header).
5. Anything about the DUT's current (2026-08-29) state beyond "silent on
   both UART and network as of 01:47:50Z" — no inference is drawn about
   cause.

## Recommended next steps

1. Physically inspect the DUT (power/LED state, seating, C6 add-on) before
   any further remote probing; do not power-cycle or reset without
   recording the pre-inspection state first.
2. Once console access is confirmed alive again, repeat S0b but gate the
   SSH trigger on a fresh ICMP-reachability check immediately beforehand, so
   the shot is not fired against an already-unreachable DUT.
3. Treat "network unreachable with a healthy console/kernel and no WDT
   reset" as its own tracked failure mode, distinct from the documented
   SSH/exec wedge, until a shot demonstrates the same signature (ICMP dies
   ~1.56 s after `exec("id")`, hard reset ~90.85 s later) **while** the
   beacon is running.
