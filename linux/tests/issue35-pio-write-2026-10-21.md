# #35 follow-up: the PIO write path (2026-10-21)

v0.4.1's validation record closed the read side of #35 (PIO reads return
the first word repeated on the P4) and left one open question: "a
transmit tail that rounds to 8 or 12 bytes still goes out by programmed
I/O.  No corruption was seen, but no test isolates it."  This record
answers that question on the v0.4.1 tree.

Board: M5Stamp ESP32-P4 + C6 (SDIO), P4 USB Serial/JTAG on COM10.  Only
the P4 flash was written, with `flash-candidate.ps1 -PreserveBoot` (-PreserveBoot was the opt-in then; since 2026-10-21 R2 preserving the boot slots is the default and the switch is a no-op; -WriteBoot writes the boot FS); the
C6 firmware was never modified or flashed and nothing was sent through
it beyond the probes below.  No SSID, PSK, password or address is
quoted here.

Branch: `rest-0.4.2` (worktree `C:/Codes/tsuyoshi-otake/m5-rest`),
based on v0.4.1 `0664323`.  The main checkout is untouched.  Commits:

- `01eaabd` → `8765837`: package patch `0044`, the opt-in write probe
  (`es_wr_probe`/`es_wr_addr`, default off)
- `b590b27`: kernel patch `0088` (write-side IDMAC bounce) + the `0044`
  CONF_W7 lane fix
- `11b8870`: stage `0088` in `build-m1.sh`'s patch list (the series is
  enumerated, not globbed; `0088` was committed but never applied in
  `rest6`), DTS comment updated
- `cf48d5a`: `0044` span guard — `es_wr_span_safe()` rejects any
  transfer covering `0x90..0x9b` (PKT_LEN), `0xbc..0xc7`, or `>=0xc8`,
  so the battery cannot take the link down at any base
- `c287a16`: `0044` restore narrows to the inert span (CONF_W8..15
  republish themselves ~4 Hz; snapshot-restoring them is useless and
  a write-write race with the slave)
- `0baa569`: `0044` restore narrows further to CONF_W6 (`0x88..0x8b`)
  only, adds a post-battery liveness read and per-byte failure
  logging, and corrects the register model: CONF_W7[0] (`0x8c`) is
  the esp-hosted host->slave doorbell — a write of `v` by CMD52 *or*
  CMD53 fires slave `event_cb` once per set bit (bit1 = esp_restart).
  `es_wr_span_safe()` now also rejects any span containing `0x8c`, so
  no probe transfer can fire a slave command at any base.
- round-1 squash (see `git log`): `0088` gains a write-side-only
  override `es_fifo_bounce_wr` (-1 follows `es_fifo_bounce`, 0 off,
  1 on) so raw PIO writes can be benched without also disabling the
  0087 read bounce the IDMAC read-back relies on; 0088/build text
  rewritten to the measured result (preventive, not a proven defect).

## Which writes take PIO

`dw_mci_pre_dma_transfer()` (kernel patch `0086`) sends a transfer to
PIO when `blocks*blksz < dma_threshold` (16 B default), `blksz & 3`,
or any sg `offset`/`length` has `& 3`.  This is direction-agnostic.
esp-hosted's TX path rounds every packet to a 4-byte count, so TX
packets of 4/8/12 bytes ride PIO under the default threshold; aligned
writes of 16 B and more take IDMAC.

## The probe

`0044` adds a default-off battery under one `sdio_claim_host()`:

- `es_wr_probe` bitmask: `0x01` incrementing-address CMD53 writes of
  len {8,12,16,20,24,28,32} at `es_wr_addr` (default `0x88`), `0x02`
  the same at 40–64 B, `0x04` fixed-address `sdio_writesb` writes of
  40–64 B at `0x88`.  Writing the parameter under
  `/sys/module/esp32_sdio/parameters/` re-runs the battery.
- Each length runs twice: "pio" writes from an unaligned buffer (sg
  `offset & 3` forces PIO at any size), "dma" from an aligned one
  (PIO below the threshold, IDMAC at/above it — the >=16 B "dma" rows
  are the write-then-read-back control).
- Non-repeating data (`seed + 7*i`, seed advances per transfer).
- Each incrementing write is verified by an `sdio_memcpy_fromio()`
  read-back plus a bytewise CMD52 oracle; compare skips read-only
  positions, counts CONF_W8..15 flips as volatile deltas (`vd`, the
  running C6 republishes that window ~4 Hz), and masks CONF_W7's even
  lanes.
- Target window: `0x88..0xc7` saved bytewise over CMD52 before the
  battery; afterwards only CONF_W6 (`0x88..0x8b`) is restored.  WIN_CMD
  (`0x84`) and TOKEN_WDATA (`0xc8`) are unreachable by construction.
- CONF_W7[0] (`0x8c`) is the esp-hosted host->slave interrupt vector:
  a write of `v` — by CMD52 or CMD53 — raises slave `slc0int` bits
  `v[7:0]` and the running firmware's `event_cb` handles each set bit
  (open, close, reset, power-save).  `es_wr_span_safe()` rejects any
  span containing `0x8c` and the restore never writes it.

## What the probe found

1. **CONF_W7 (`0x8c..0x8f`) retains only its odd byte lanes.**  A CMD52
   write of `ef be ad de` reads back `00 be 00 de`; a CMD53 write shows
   the identical pattern — a slave register semantic, not a transport
   artifact.  The compare masks those lanes (`ES_WR_LANES`).

2. **PIO writes to writable registers are byte-correct.**  On the
   unpatched build (rest5, no write bounce), 8/12 B PIO writes to
   `0x88` delivered every writable byte exactly (both the IDMAC
   read-back and the CMD52 oracle agreed once the CONF_W7 lanes and RO
   registers are accounted).

3. **A write spanning `0x94..0x97` (PKT_LEN1) wedges the SDIO
   function** — on either transport.  On rest5 the first `>=0x94`
   write (16 B PIO) completed and every later command timed out until
   reset (`-110`, including CMD52).  On rest7, with *all* writes going
   through IDMAC (bounce or direct), the link died at the same point —
   the first write reaching `0x94` again completed, and the next timed
   out.  This is a C6-side hazard on host writes to the packet-length
   accounting registers: a probe-only concern, since production
   traffic writes packet RAM (`0x1f800` window), never SLCHOST
   registers.  `es_wr_span_safe()` now skips any transfer whose span
   covers `0x90..0x9b`, `0xbc..0xc7` or `>=0xc8` and logs `SKIP`, so the
   battery cannot take the link down at any base.

4. **The `0088` write bounce works end-to-end.**  rest7 logged `#35:
   first multi-word PIO write bounced to IDMAC (8 bytes)`; every PIO-
   bound write (unaligned source at any size, aligned <16 B) was
   staged into the existing bounce buffer and submitted through IDMAC.
   No `does not pop` warning for any write.

5. **CONF_W7[0] (`0x8c`) is a live command channel — on CMD52 *and*
   CMD53.**  A write of `v` fires the slave's `event_cb` once per set
   bit: bit0 open, bit1 close→`esp_restart()`, bit2 `sdio_reset`,
   bit3/4 power-save.  Measured: CMD52 `0x02` at `0x8c` rebooted the
   C6 and dropped the link until P4 reset; CMD52 `0x40` (unassigned)
   was ignored; a CMD53 write of `0xaa` covering `0x8c` timed out
   mid-transfer — the C6 restarted on bit1 before it could answer.
   `0x8c` never retains (always reads 0).  This also explains the
   rest5-boot2 "spontaneous" link death: the `0xdeadbeef` CMD52
   discriminator write set bits 0–3 on the vector.

6. **The battery's random bytes could fire the doorbell (root cause
   of the earlier post-battery wedges).**  An incrementing write at
   base `0x88` spans `0x8c`; whatever pattern byte lands on it fires
   the command bits it happens to set.  On rest10 `0xe9` landed —
   bit3 (power-save) blocked the slave's event context: commands and
   register probes still answered (`liveness ret=0`, restore `ret=0`)
   but every later TX failed (`tx_process -110`, gateway unreachable)
   while `=5` at `0x9c` — which never touches `0x8c` — left the data
   path up on the same boot.  On rest8/9 the same `=7` battery put
   bytes with bits 1/2/4 on the vector (~62% chance per write): the
   C6 restarted mid-probe and the CMD52 restore found a dead link.
   `es_wr_span_safe()` now rejects every span containing `0x8c`, so
   no probe transfer can fire a slave command at any base.
   Production traffic never writes SLCHOST registers — host TX goes
   to packet RAM (`0x1f800` window) — so this was a probe-harness
   hazard, not a production-path defect.

## Results

Build `rest5` (probe only, no write bounce), `es_wr_probe=7` at `0x88`:

- len 8/12, both paths: every writable byte correct; `mm=2/8` rows were
  the unmasked CONF_W7 even lanes (point 1, fixed in `0044`).
- len 16 "pio": `wret=0`, IDMAC read-back correct, then the CMD52
  oracle timed out and the link stayed dead (`restored rw bytes
  ret=-110`).

Build `rest7` (probe + `0088`), `es_wr_probe=7` at `0x88`:

- len 8/12/16 all rows: `mm=0/6 vd=0` both oracles — the formerly fatal
  16 B size now completes cleanly through the bounce.
- The `len=16 dma` row (`wret=-110`) is where the PKT_LEN-spanning
  write (point 3) wedged the link — on IDMAC, proving it is not a FIFO
  issue.

Build `rest7`, `es_wr_addr=0x9c`, `es_wr_probe=5` (short at `0x9c`,
fixed at `0x88`):

- len 8–32 incrementing, both paths: `wret=0`, `mm=0` `vd=0` on every
  row — `>=16` B writes are safe when the span misses PKT_LEN.
- fixed 40–64 B at `0x88`: `expect0 == dma b0 == c52 b0` on every row.

Build `rest8` (probe + span guard), `es_wr_probe=7` at `0x88`:

- len 8 both paths: `wret=0`, `mm=0/6 vd=0` both oracles.
- len 12–64 incrementing rows: all `SKIP unsafe span` — the guard
  refused every PKT_LEN-crossing write; the link stayed up through
  the whole battery.
- fixed 40–64 B at `0x88`: `expect0 == dma b0 == c52 b0` on every row.
- The 40-byte CMD52 restore pass (CONF_W6 + CONF_W7 + CONF_W8..15)
  ended `ret=-110`: the link died after the battery — finding 6.
  `wlan0` stayed `UP,LOWER_UP`; the C6 stopped answering commands.

Build `rest9` (restore narrowed to inert span), `es_wr_probe=7` at
`0x88`: every data row clean as above; the inert-only restore still
ended `ret=-110`, and the post-mortem `es_reg_probe` CMD53 read
returned `-110` — the wedge is in the slave, not a specific write.
Doorbell discriminator (separate boots, same image): CMD52 `0x40` at
`0x8c` harmless; CMD52 `0x02` at `0x8c` → immediate link death
(finding 5); CMD52 `0x02` at `0x8d` harmless (doorbell is byte 0
only).

Build `rest10` (CONF_W6-only restore + liveness + per-byte logging),
`es_wr_addr=0x9c`, `es_wr_probe=5`:

- len 8–32 incrementing, both paths: `wret=0`, `mm=0` `vd=0` on every
  row (one `vd=17` CMD52 oracle delta at len 20 — a republish race
  mid-read, flagged as volatile, not a mismatch).
- fixed 40–64 B at `0x88`: `expect0 == dma b0 == c52 b0` on every row.
- `post-battery liveness ret=0`, `restored inert bytes ret=0`; a
  gateway ping after the battery still answered.

Same build, `es_wr_addr=0x88`, `es_wr_probe=7`: len 8 wrote pattern
byte `0xe9` onto `0x8c` → bit3 (power-save) — probe completed
(`liveness ret=0`, `restored ret=0`, `mm=0` everywhere) but the data
path was dead afterwards: `tx_process -110`, gateway unreachable —
finding 6.  A following `es_reg_probe` CMD53 write of `0xaa` to `0x8c`
then dropped the command channel too (`ret=-110`), confirming the
vector fires on CMD53 writes as well.

Also on rest10: the first len-8 row's CMD52 oracle reported `mm=2/6`
(`cd ff ff e2` where `d4 db` was written) — the slave itself rewrote
CONF_W6[1,2] between the IDMAC read-back and the oracle; CONF_W6 is
mostly inert but not perfectly static on this firmware.

Build `rest11` (0x8c span guard added) — the final image, flashed
with the P4-only gate (`C6 write: none`, all five partitions verified
by digest), boot log `rest11-boot.log` sha256
`85bd25c7fa751fc4a5580547158f3389b8b23e5cd51b013669bee7569402c1c3`.

`es_wr_probe=7` at default `0x88`:

- All 22 incrementing rows (8–64 B, both paths): `SKIP unsafe span` —
  every span from `0x88` with len >= 8 covers the `0x8c` doorbell, so
  the guard refused them all.  No byte ever reached the vector.
- fixed 40–64 B `sdio_writesb` at `0x88` (fixed address never covers
  `0x8c`): all 8 rows `wret=0`, `expect0 == dma b0 == c52 b0`.
- `post-battery liveness ret=0`, `restored inert bytes ret=0`.

`es_wr_addr=0x9c`, `es_wr_probe=5` (same boot):

- len 8–32 incrementing, both paths, all 14 rows: `wret=0`,
  `mm=0/N vd=0` on the IDMAC read-back *and* the CMD52 oracle —
  including the 16–32 B sizes and the PIO-forced (unaligned source)
  rows, which the `0088` bounce submitted through IDMAC.
- fixed 40–64 B at `0x88`: `expect0 == dma b0 == c52 b0` on all 8 rows.
- `post-battery liveness ret=0`, `restored inert bytes ret=0`.

Normal-load check on the same boot, immediately after both probe
runs: gateway ping OK, then five consecutive 10 MB `wget` downloads
over wlan0 — all `OK` — then gateway ping OK again.  Post-load dmesg
shows no `does not pop`, no `-110`, no `tx_process` error, no
`esp_restart`: the guarded battery leaves the data path fully
functional, and the `0088` bounce + normal IDMAC path carried the
whole load without a single FIFO fallback warning.

Build `rest12` (`0088` + `es_fifo_bounce_wr`), flashed with the
P4-only gate (`C6 write: none`, all partitions verified), boot log
`rest12-boot.log` sha256
`98b06da3f91696d360866b176ca2ba45c312c742e5fa3cdbace68b2348b9019e`.

Raw-PIO run — `es_fifo_bounce_wr=0` (write bounce off, the 0087 read
bounce stays on so the IDMAC read-back remains trustworthy),
`es_wr_addr=0x9c`, `es_wr_probe=5`:

- len 8–32 incrementing, **raw PIO** rows (the `does not pop`
  rate-limited warning fires for each, confirming the transfers
  really went through the CPU FIFO): `wret=0`, `mm=0/N vd=0` on the
  IDMAC read-back *and* the CMD52 oracle — all 14 rows including
  16–32 B.  This is the run earlier builds could not produce: multi-
  word raw PIO writes are byte-correct at a safe address.
- fixed-address `sdio_writesb` 40–64 B at `0x88`, raw PIO: the CMD52
  oracle shows every write landed correctly (`expect0 == c52 b0` on
  all 4 rows).  The IDMAC read-back taken immediately afterwards
  returns the *previous* transaction's byte (`b0` lags one row) — a
  write-drain/visibility lag of CPU-FIFO writes with respect to an
  immediately-following IDMAC read, not data loss; the DMA-write
  control rows show no lag.  Worth knowing if a driver ever mixes
  PIO writes with immediate IDMAC reads of the same window.
- `post-battery liveness ret=0`, `restored inert bytes ret=0`;
  gateway ping alive after the run.

## Conclusion

The open #35 question is answered: **host-to-C6 PIO writes are
byte-correct** — short writes (<16 B, or unaligned at any size)
deliver every writable byte exactly, verified by two independent
read-back oracles, including a raw-PIO run with the write bounce
disabled (rest12).  The write side never had the read side's
FIFO-doesn't-pop defect; the `0088` write bounce is still carried as
cheap insurance under the same `espressif,fifo-no-pop` DT flag
because the slave silently tolerates IDMAC for every size tested, and
it removes the last PIO path from a FIFO whose pop behaviour is
already known-broken on reads.  Every wedge seen during this work was
a *target-register* hazard (PKT_LEN writes; random bytes landing on
the `0x8c` doorbell), not transport corruption — and none is
reachable from production TX, which writes packet RAM.  The probe is
default-off, self-guarding, and leaves the link alive.
