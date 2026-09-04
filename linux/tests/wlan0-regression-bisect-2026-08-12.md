# `wlan0` regression bisect: build #29 → #30

Status: **analysis complete. Test A0 has since been run, and it confirmed the
central claim below.** No hardware was touched to produce *this* document —
every number in it is measured from artifacts and captures already on disk under
`C:\Users\developer\tmp`, and nothing here was flashed, probed or rebuilt.

The A0 result, the root cause it licensed, and the reconstruction patch are
recorded separately in
[`a0-lastgood-restore-and-rootfs-bisect-2026-08-12.md`](a0-lastgood-restore-and-rootfs-bisect-2026-08-12.md):
restoring the six preserved regions verbatim returned `Tx Pos = 10`,
`BOOT_CMD53_RX: len=40`, the boot-up event, `NG-1.0.6.0.1` and `wlan0`, and the
cause is a register-access refactor from byte-wise CMD52 reads to a single
4-byte CMD53 register read. Read this document for how the regression was
narrowed to the `ESP_SLAVE_TOKEN_RDATA` read; read that one for what was
actually wrong. Where the two differ in a count, that one is measured later and
wins.

Question this answers, as posed: *what was different in the historical Linux
environment that allowed the C6 to produce the nonzero interrupt and 40-byte
boot frame, when the same C6 reset waveform and current reconstruction do not?*

## 1. The answer, stated first

**The divergence is upstream of the interrupt path, and the interrupt path was
never the right place to look.**

The earliest C6-produced value the host reads already differs, and it is read
before anything the investigation has been instrumenting:

| Observable | Success (#29) | Every failure since (#30 …) |
|---|---:|---:|
| `get_firmware_data: Tx Pos` | **10** | **0** |
| `ESP_SLAVE_TOKEN_RDATA` bits 27:16 (derived) | **20** | **≤ 10** |

From `esp_hosted_ng/host/sdio/esp_sdio.c:310-353`:

```c
ret = esp_read_reg(context, ESP_SLAVE_TOKEN_RDATA, (u8 *) val, sizeof(*val), ACQUIRE_LOCK);
*val = ((*val >> 16) & ESP_TX_BUFFER_MASK);
if (*val >= ESP_MAX_BUF_CNT)
        context->tx_buffer_count = (*val) - ESP_MAX_BUF_CNT;   /* 20 - 10 = 10 */
else
        context->tx_buffer_count = 0;                          /* any value <= 10 */
esp_info("Tx Pos ======  %d\n", context->tx_buffer_count);
```

`get_firmware_data()` is called from `init_context()` (line 364), i.e. from
`esp_probe` **before** the `OPEN_DATA_PATH` write, **before** the ISR is
registered, and **before** any line of patches 0009, 0010 or 0011 can execute.

That register is written by the C6 and only read by the host. Reading 20 means
the C6 application firmware had come up far enough to publish its SDIO-slave TX
buffer pool. Reading ≤10 means it had not — while its SDIO **slave peripheral**
still answers CMD52/CMD53 and completes enumeration, because that part responds
without the application running.

So the sequence to explain is not `OPEN_DATA_PATH → no interrupt`. It is
`enumeration succeeds → C6 firmware has not initialised → therefore no
interrupt, no boot frame, no wlan0`. Every symptom downstream follows from the
one register.

### Correlation across all 173 captures

| Relation | Holds? |
|---|---|
| `BOOT_CMD53_RX: len=40` present ⇒ `Tx Pos = 10` | **yes, no exceptions** |
| `Tx Pos = 0` ⇒ no boot frame | **yes, no exceptions** |
| `Tx Pos = 10` ⇒ boot frame | no — `m3-pio-cmd53-final` has `Tx Pos=10`, `len40=0` |

`Tx Pos = 10` is therefore a **necessary but not sufficient** condition. Stated
that way deliberately: the PIO-era captures (`m3-pio-boot`, `m3-real-pio`,
`m3-boot-poll`, `onboard-sdio-registers`) read `Tx Pos = 630`, i.e. a raw field
of 640 — a garbage read from an era when CMD53 itself was broken. The register
has produced three distinct classes of value, and "0" is only one of them.

### What the driver's own logging destroys

The `else { tx_buffer_count = 0; }` branch collapses every raw value from 0 to
10 into the printed `0`. So the log cannot distinguish:

- the C6 published **nothing** (raw 0), from
- the C6 published a **different, smaller** count (raw 1–10).

Those two have different causes. §14.2 — the log only proves what it prints, and
this one prints a clamped value. Test A1 below exists solely to remove the clamp.

## 2. The bisect is exact

Build numbers reset when the Buildroot tree is cleaned (two distinct `#2` builds
exist on 2026-08-10), so the identifying key is the embedded UTC compile stamp,
not `#N`.

| | Last good | First bad |
|---|---|---|
| Build | `#29 Mon Aug 10 10:48:40 UTC 2026` | `#30 Mon Aug 10 10:56:43 UTC 2026` |
| Capture | `m3-cmd53regs` 20:03:29 JST | `m3-sdioagg-boot` 21:55:21 JST |
| `Tx Pos` | 10 | 0 |
| `len=40` frames | 8 | 0 |
| Preserved set | `easystick-p4-m3-lab-resetpin-20260810/` | `easystick-p4-m3-sdioagg-20260810/` |

The success is not a one-off: **33 captures contain `BOOT_CMD53_RX: len=40`**,
spanning 15:30:17 → 20:03:29 JST across builds `#2`(06:01:41) through `#29`.
It closes permanently at 20:03:29; the first failure is 21:55:21.

### Region-by-region diff of the two preserved sets

| Region | Flash offset | Last good (#29) | First bad (#30) | Same? |
|---|---|---|---|---|
| `bootloader.bin` | 0x2000 | `681e730b…` | `681e730b…` | **identical** |
| `partition-table.bin` | 0x8000 | `580a0ca9…` | `580a0ca9…` | **identical** |
| `boot-shim.bin` | 0x10000 | `3e90e8b0…` | `3e90e8b0…` | **identical** |
| `easystick-stamp-p4.dtb` | 0xf10000 | `a4284b33…` | `a4284b33…` | **identical** |
| `Image` | 0x90000 | `6ee6e984…` | `bc1ba145…` | differs — **66 bytes, all banner** |
| `rootfs.squashfs` | 0x810000 | `a615f3c9…` | `9f96804f…` | **differs** |

The `Image` difference was measured byte-for-byte, not assumed:

```
sizes: 6497284 vs 6497284 (delta 0)
differing bytes: 66 of 6497284 (0.001016%)
contiguous diff regions: 6
  container hostname  ecd0a22cfb9a → b5c615c99f22   (x3, 12 B each)
  version stamp       #29 … 10:48:40 → #30 … 10:56:43  (x2, 22 B each)
  GNU build-id note                                  (x1, 20 B)
```

Every differing byte lies inside a version string or the build-id note. **There
is no functional kernel difference between the last success and the first
failure.** The two kernels are the same code with a different name.

That leaves `rootfs.squashfs` — which carries `esp32_sdio.ko` — as the sole
functional change across the regression boundary.

## 3. What this eliminates, by measurement

Each axis raised in the investigation, and what closed it:

| Axis | Verdict | Evidence |
|---|---|---|
| GPIO42 ownership | **eliminated** | `boot-shim.bin` byte-identical; `Triggering ESP reset` appears in **0 of 173** captures — the driver has never once driven the pin |
| GPIO42 timing | **eliminated** | same identical shim binary across the boundary |
| `resetpin` module default | **eliminated** | `warn42=2` present in *both* #29 and #30. `esp_reset()` skips the entire body when `resetpin == -1` and prints-then-skips when `gpio_is_valid(42)` fails — neither touches a GPIO |
| DTS / device tree | **eliminated** | `easystick-stamp-p4.dtb` byte-identical |
| `max-frequency = <400000>` | **eliminated** | identical DTB; both captures log `Bus speed (slot 1) = 40000000Hz (slot req 400000Hz, actual 400000HZ div = 50)` verbatim |
| 4-bit bus width | **eliminated** | identical DTB, identical bus line |
| DW-MMC DMA vs PIO | **eliminated** | identical DTB; node selection is a DT property |
| Bootloader / partition table | **eliminated** | both byte-identical |
| Kernel / DW-MMC patch set | **eliminated** | 66-byte banner-only Image delta |
| Kernel configuration | **eliminated** | same reason — a config change would move far more than 66 bytes |
| IRQ claim / enable ordering | **not reached** | the divergence precedes ISR registration |
| C6 firmware | **eliminated** | all OTA activity 11:18–11:34 JST, **hours before both** the last success (20:03) and the first failure (21:55). Every success reports `ESP-Hosted Version: NG-1.0.6.0.1`; `Slave firmware version: 2.12.1` appears only in pre-OTA `host_performs_slave_ota` output |
| C6 SDIO slave identity | **eliminated** | all three CIS tuples byte-identical across eras, including `0x1b [c1 41 30 30 ff ff ff ff]` |
| **ESP-Hosted host driver** | **the only survivor** | rootfs is the sole functional delta |

Two things that look like discriminators and are not:

- `probe with driver esp32_sdio failed with error -22` appears in the **success**
  capture too, then `wlan0` appears anyway. It is not a failure signal.
- `SDIO IRQ received: status=0x00000000` also appears in the **success** capture,
  later in the session. Zero-status interrupts are normal; what matters is that
  the *first* one carried `0x00800000`.

## 4. The test that was never run

The `wlan0-historical-recon` and `reset1500` experiments restored the shim
correctly and then paired it with the wrong rootfs.

Shim identity is recoverable from the pre-handoff ESP-IDF timestamps:

| Shim | GPIO42 released | Handoff | Waveform |
|---|---:|---:|---|
| `3e90e8b0` (historical) | `I (641)` | `I (1521)` | ~137 ms total — matches the stated 1 ms / 20 ms / 100 ms |
| `9d1a25bd` (current) | `I (2041)` | `I (2921)` | +1400 ms — the `C6_RESET_READY_DELAY_US = 1500000` in `boot-shim/main/m2_sdmmc.c:58` |

Both hold GPIO42-release → Linux handoff at a constant **880 ms**.

`wlan0-historical-recon-cold` releases at `I (641)`, so it *did* run the
historical shim. But it reports build `#30`, and every 08-11 candidate set
carries rootfs `9f96804f…`:

| rootfs | preserved sets | outcome |
|---|---:|---|
| `9f96804f0d6a` | **6** — `m3-sdioagg`, `m3-lab-live-ttygs1`, `m3-lab-hosted-rebuild`, `reset1500-candidate`, `pio-candidate`, `diag-candidate` | all fail, `Tx Pos = 0` |
| `a615f3c99a0d` | **1** — `m3-lab-resetpin-20260810` | the only set that ever produced `wlan0` |

**The last-good rootfs has not been written to the board since 20:03 JST on
2026-08-10.** Every subsequent experiment — reset waveform, PIO, diagnostics,
historical reconstruction — varied something else while holding the one artifact
that actually changed at the regression boundary fixed in its broken state.

Note the directory name is stale and misleading: `m3-lab-resetpin-20260810`
refers to a 12:38 experiment, but its contents were repopulated at 19:49 with
build #29. The embedded compile stamp is the invariant; the directory name is a
per-directory convention (§14.9). Identify these sets by stamp, never by name.

## 5. Recoverable vs. permanently missing

**Recoverable — complete and flashable now:**

`C:\Users\developer\tmp\easystick-p4-m3-lab-resetpin-20260810\`, all six
regions present, this is the exact byte-for-byte state that produced `Tx Pos=10`
/ `len=40` / `wlan0`:

```
681e730b7aa47eb2463cbb9fd48ece12c5f9df7e76b999cbeec5f3dc67b056de  bootloader.bin        @0x2000
580a0ca942e6746a768724ae7bbb486dee47a228ca46b477ee598ac45738e87f  partition-table.bin   @0x8000
3e90e8b01bf2f347d6fd68c6fd3f67409b79c33fee2043773b46543ce44df418  boot-shim.bin         @0x10000
6ee6e9844818f5fee8ed0fcf4ff1cf720caf0a99da7e443717eefad0fc59e53a  Image                 @0x90000
a615f3c99a0d9c54c421399dfdc4806ce6790b32f740ce62730e583481849538  rootfs.squashfs       @0x810000
a4284b333bfee614c6f41e7ed8566a910af7eca9916cff750a06c62fc439b0f7  easystick-stamp-p4.dtb @0xf10000
```

**Permanently missing:**

- **The pre-22:03 JST source tree.** Git's first lab commit (`2154b2f`,
  2026-08-10 22:03 JST) postdates the break, so the source that built the
  last-good rootfs is not in history. For *this* question the binaries are the
  stronger evidence anyway — they are what actually ran.
- **Which change inside the rootfs is responsible.** `unsquashfs` is not
  available in this environment, and a raw byte-diff is useless: squashfs
  compresses per block, so one changed module cascades into 75.9% of bytes
  differing from the first altered block onward (offset 0x64996, where a GNU
  build-id note differs — the signature of a rebuilt `.ko`). The rootfs delta is
  **not provably limited to patch 0009**, only to "something in the rootfs".
- **Linux-side timing.** No capture has `CONFIG_PRINTK_TIME`, so no kernel line
  carries a timestamp. The requested intervals between enumeration, IRQ claim
  and `OPEN_DATA_PATH` are **not recoverable** from any existing log. Only the
  pre-handoff ESP-IDF `I (nnnn)` stamps exist, and those are settled by the
  identical shim hash.
- **The intermediate `#2 Mon Aug 10 12:48:43 UTC 2026` build's inputs** (a
  cleaned-tree rebuild at 21:48 JST, three minutes after patch 0009 was
  authored). Its rootfs `ec76bd4b…` survives in `m3-cfg80211r13-20260810/`;
  its source does not.

## 6. Proposed A/B tests, smallest first

All four are pure P4-software; none requires probing, wiring, or any C6 write.

**A0 — restore the last-good set verbatim.** Flash all six regions of
`easystick-p4-m3-lab-resetpin-20260810/` via `flash-candidate.ps1`. This is a
one-variable binary bisect using artifacts already on disk, against a boundary
where four of six regions are provably identical.

- Confirms `Tx Pos=10` / `len=40` / `wlan0` return ⇒ the fault is entirely
  P4 host software, and the C6 is exonerated for a second time.
- Fails ⇒ something outside all six flash regions changed between 20:03 and
  21:55 JST on 2026-08-10 — i.e. C6 or board state — which is a *different and
  much more valuable* finding than any further host-side change could produce.
- Take a fresh full 16 MiB readback first; no preserved readback contains the
  currently-flashed image.

**A1 — unclamp the token register.** Log the raw 32-bit `ESP_SLAVE_TOKEN_RDATA`,
`INT_RAW` and `INT_ST` alongside the derived values, instead of the
`≤10 → 0`-clamped `tx_buffer_count`. Distinguishes *C6 published nothing*
(raw 0) from *C6 published a smaller count* (raw 1–10) from *recovered*
(raw 20). Run whichever way A0 goes: it is the only way to read the discriminating
value at all, and it costs one `esp_info`.

**A2 — cross the pair.** Only if A0 recovers. Flash last-good `Image` +
first-bad `rootfs`, then the reverse. Given the 66-byte banner-only `Image`
delta the outcome is near-certain, so this is a check on the measurement, not on
the hypothesis — run it only if A0's result is surprising.

**A3 — revert patch 0009 alone** from the current tree and rebuild. This is the
narrowest source-level test, but it rests on the unproven assumption that 0009
is the whole rootfs delta. Rank it after A0/A1 for that reason.

## 7. Defects found along the way, independent of this regression

In `0009-easystick-sdio-rx-aggregate.patch`:

1. `context->rx_q` is dereferenced **before** the `!context` NULL check:

   ```c
   context = adapter->if_context;
   skb = skb_dequeue(&(context->rx_q));   /* deref */
   if (skb)
           return skb;
   if (!context || !context->func) {      /* check, too late */
   ```

2. The hunk adding `rx_q` is contextless and hand-authored
   (`@@ -79,0 +80 @@`, a §14.1 provenance tell). Against the real
   `esp_sdio_decl.h:76-83` it inserts the field **mid-struct**, between
   `tx_q[]` and `rx_byte_count`, shifting three trailing `u32` offsets.

Neither can explain `PACKET_LEN=0`: for the boot frame (len 28, offset 12 ⇒
`frame_len` 40, `len_from_slave` 40) 0009's aggregate path returns the frame
unchanged. And both live downstream of `init_context()`, where the divergence
already exists. They are real bugs; they are not this bug.

An unexplained inconsistency in the current capture, recorded rather than
resolved: `dispatch=1` together with `raw=0x00000000 raw_ret=0` — an SDIO card
interrupt was dispatched to the host while the slave reports no raw interrupt
source. Consistent with the C6's slave peripheral being alive while its
firmware is not.

## What this analysis cannot see

- It cannot identify *which* rootfs change is responsible (§5), only that the
  responsible change is in the rootfs.
- It cannot measure Linux-side timing in any historical capture; no capture has
  kernel timestamps.
- It says nothing about *why* the C6 would stop publishing its buffer pool. It
  bounds the cause to the P4 rootfs **on the assumption that the C6 and board
  are unchanged** — an assumption A0 is designed to test rather than inherit.
- `Tx Pos` correlation is drawn from log text across 173 captures, not from a
  register trace. A1 is what turns it into a measurement.
