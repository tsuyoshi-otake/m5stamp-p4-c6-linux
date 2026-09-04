# SSH wedge / Instrument C beat isolation

Date: 2026-09-02 JST  
Evidence root: `C:\Users\developer\tmp\p4-ssh-wedge-stacktrace-20260829`  
DUT: ESP32-P4 RV32 NOMMU Linux / Buildroot m3-lab, `10.255.10.161`, COM10

## Result

- Named SSH-wedge cause: **OPEN**.
- Class: exec-triggered loss of kthread / scheduler forward progress; MWDT then fires on the leftover timeout.
- `+90 s` epoch shift (184.5 s → 274.5 s): **0054 crash capsule**, not beat period and not beat-kthread existence.
- 0054 does not cause the wedge. It moves when the WDT reset fires.
- First in-window beat on the 184.5 s fault: cpu0 `seq=1023` (100 ms) — beat stopped near exec, not at reset.

## Isolation table (delay110, BYTE-IDENTICAL to r1/r3)

Serial A `id`, no COM10 write after A, idle to reset+110 s, Paramiko exec `id`, 240 s hold, no ICMP during B. P3 window `[184.44, 184.62]`. Outside that band: VOID, do not interpret beat Delta.

| Image | 0054 | beat | TX ledger | delay110 rst | Window | Capture SHA-256 |
|---|---|---|---|---|---|---|
| `368a08f0` TX-ledger confirm | no | no | on | reset+184.578 / exec+73.5 | IN | `b8e58100…` |
| `7c09ac61` 10 ms C | yes | 10 ms | off | 274.516 / 163.688 | VOID | `564a738b…` |
| `886a9e70` 100 ms C | yes | 100 ms | off | 274.547 / 163.89 | VOID | `1c333181…` |
| `7ea59fb6` nobeat | yes | no | off | 274.61 / 163.985 | VOID | `c828a910…` |
| `33c25fab` nocapsule | no | no | off | reset+184.516 / exec+73.922 | IN | `ef8477d6…` |
| `60a71fe3` 0058 beat, no 0054 | no | 100 ms | off | reset+184.61 / exec+73.75 | IN | `7fab8985…` |

`TX ledger 0/0/0` on every C image except `368a08f0` means the ledger patches were **off**, not that exec produced zero TX dequeues. Do not read those zeros as a TX-path finding.

Do not overwrite Image SHAs `175a4ad8` (control) or `368a08f0` (TX-ledger).

## 60a71fe3 gates

- Contract: `BB=1 TX=0 FORCE_PIO=0 NC=1 ALLOW_NC=1 CAPSULE=0 BEAT=1`.
- Boot-shim SHA-256 `01b35253e8183b9c041d9cbee39d7a33f29fb03ee3b37fcafb2266ca73123464`.
- Beat slot: BB PA `0x50108080` + `0x120` + CMD52 hole `0x18` = `0x501081b8`.
- P0 `test_no_feed`: **VOID**. That module parameter exists only in 0054 (`sh: can't open .../test_no_feed`). Capture SHA-256 `4743afcfe5ceca647369b7c01522ca16a2bed74bf1c37641b37edde251360716`.
- P2 idle 240.75 s: **PASS** (no rst:0x7; dump cpu0 `seq=2509`). Capture SHA-256 `cb154d6ed81c3ac97e018b98ea2d9ef21a154dc2979571c1293dfdabdc40b8db`.
- P3: `SSH_ID_WEDGE`, capsule EMPTY, `Using_DMA` true, TX ledger 0/0/0. cpu0 `seq=1023` `beat_jiffies=4294919229` `last_feed_jiffies=4294907646`. cpu1 slot is garbage (`cpu=716`; Image is UP).

## 0054 capsule on the three VOID delay110 shots

Same recovery captures, binary needles:

| Image | Capture | PRETIMEOUT | CAPSULE_COMMIT | VALID | empty |
|---|---|---|---|---|---|
| `7c09ac61` | `c-bb-p3-delay110-20260901.bin` | 0 | 0 | 0 | 2 |
| `886a9e70` | `c-bb-p3-delay110-100ms-20260902.bin` | 0 | 0 | 0 | 2 |
| `7ea59fb6` | `c-bb-p3-delay110-nobeat-20260902.bin` | 0 | 0 | 0 | 2 |
| `886a9e70` P0 `test_no_feed` | `c-bb-p0-test-no-feed-100ms-20260902.bin` | 2 | 2 | 1 | 1 |

The same 0054 Image that writes a VALID capsule when feeds are stopped with IRQs live writes EMPTY on the SSH wedge, with no PRETIMEOUT line in the 30 s grace window. IRQ-enabled busy loop is almost eliminated. Remaining classes: IRQ-off / higher-priority CLIC sit, or core/bus hang.

## CMD53 `request_end` `idle=1` semantics

Sticky generation tag, not a counter. `stage_request_end_*` hold the generation that last marked them. Dump prints 1 iff `stage == generation`. `begin()` does not clear old words.

`enter=1 before_next=0 after_next=0 idle=1` means the last armed FOCUS_ARG (`0x97ec0000`) `dw_mci_request_end()` entered, skipped the queued-next arm, and took `STATE_IDLE`. That focused transfer left the host idle. It is not a live "host is idle right now" flag. Later non-FOCUS_ARG MRQs are invisible. So the wait that wedges the kernel is **not** a still-open focused CMD53/CMD52 completion. Candidates that still fit (IRQ-off + host idle + WDT-only) include IDMAC/cache complete-flag polls (`EASYSTICK_IDMAC_NONCOHERENT_RING=1`) and long `dw_mci_wait_while_busy` / reset polls. PIO A/B (`SDIO_FORCE_PIO=1`) remains the cheap cut if JTAG does not name the site.

## S0b ICMP "revival" is post-reset association

Evidence: `C:\Users\developer\tmp\easystick-p4-s0b-console-beacon-20260829\`.

| Event | UTC | Source |
|---|---|---|
| `EXEC_SENT` | 2026-08-29T02:22:51.069Z | measured |
| ICMP dies | 2026-08-29T02:22:52.755Z (elapsed 6.0) | `s0b-icmp-liveness.jsonl` |
| last `ESLIVE` / rst:0x7 | ~2026-08-29T02:23:01.565Z | extrapolated beacon + next UART line is ROM/`HP_SYS_HP_WDT_RESET` |
| ICMP returns | 2026-08-29T02:24:31.808Z (elapsed 105.063) | jsonl |

rst → ICMP ≈ **90.2 s**. That is boot + Wi-Fi association after `rst:0x7`, not the same kernel recovering. Close the S0b "mysterious ICMP revival" item.

This 90 s is **not** the 0054 epoch shift (184.5 → 274.5 from boot). Same digit, different interval.

S0b build: 0054 was present (`wdt_stage0_irq_s: 90`, empty capsule). Same-day addendum shows that Image still had the pre-fix `CONFIG3=grace_ticks` programming. exec→rst on that long-uptime shot was ~10.5 s (uptime 745 → 756), leftover from that kernel, not today's delay110 73.5 / 163.7 windows.

## Injector expected signatures (0059)

`echo 1|2 > /sys/module/esp32p4_wdt/parameters/inject` (root, 0200). Independent of `test_no_feed`.

| mode | IRQs | expected 0054 Image |
|---|---|---|
| 1 | on | `INJECT` line, then PRETIMEOUT + CAPSULE_COMMIT + VALID, `epc` in `easystick_wdt_inject_spin`, `rst:0x7` |
| 2 | `local_irq_disable` | `INJECT` line, no PRETIMEOUT, no CAPSULE_COMMIT, capsule EMPTY, `rst:0x7` |

Reset time is from **last feed**, not from a magic 163.7 s. The INJECT line prints `last_feed_age_jiffies`. If inject is at the same leftover as delay110 exec (~50–60 s after last keepalive), 0054 should land near inject+163.7 s and the no-0054 / grace leftover near inject+73.5 s. A match of mode 2 to the wedge signature (EMPTY, no PRETIMEOUT, same leftover) supports IRQ-off. A miss pushes toward hang (class 3).

## Next shots

1. Injector (a) then (b) on a 0054 Image — signature match before more beat work.
2. JTAG halt after `EXIT_TIMEOUT` on a separate delay110 shot — see `ssh-wedge-jtag-halt-plan-2026-09-02.md`. halt+pc ⇒ class 2; halt does not complete ⇒ class 3.
3. PIO A/B only if JTAG does not name the site.

## What this still cannot see

- Why exec stops kthread forward progress.
- C6 slave state, skb tailroom, encrypted 802.11 payload.
- Naive `delta ≒ 120 s` P0 gate (u32 wrap, HZ=100, beat may run until reset). Use `seq` and the P3 window, not that delta.
- Live DW-MMC host state after the last FOCUS_ARG (idle bit is last focused path only).
