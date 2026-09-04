# A0: restore the preserved last-good set, and the rootfs bisect it licensed

Status: **A0 complete and classified as a full restore. Root cause identified and
now partially confirmed on the current stack.** The reconstruction patch has been
built, flashed and run: it restores `Tx Pos = 10` and the 40-byte boot frame
exactly as #29 had them, but **not `wlan0`** — the frame arrives at the right
length with a payload that repeats one 4-byte unit. Classification, the payload
comparison, and the residual difference from #29 are in §9.8. The C6 was neither
reflashed nor probed at any point.

Companion document: `wlan0-regression-bisect-2026-08-12.md` narrows the
regression to build #29 → #30 and to the `ESP_SLAVE_TOKEN_RDATA` read. This one
executes the restore that confirms it and finds what changed underneath.

## 1. Result, stated first

Restoring the six preserved last-good P4 flash regions verbatim **restored the
historical behaviour completely, through to `wlan0`.** Under the branch rule set
for A0, that is the "regression is entirely within the P4 host software/artifact
state" branch, and the rootfs bisect it authorises has since found the cause:

> A 4-byte read of an ESP SDIO slave register changed from **four CMD52
> `IO_RW_DIRECT` transfers** to **one CMD53 `IO_RW_EXTENDED` transfer**. The C6
> register window returns zeros for the CMD53 form on this host, so
> `TOKEN_RDATA`, `INT_ST` and `PACKET_LEN` all read 0.

Every downstream symptom follows from that one change: `Tx Pos = 0`,
`PACKET_LEN = 0`, no boot frame, no `wlan0` — while the slave still enumerates
normally, because enumeration is answered by the C6's SDIO slave peripheral and
does not require its application firmware.

**Qualified by the 0012 hardware result (§9.8).** Reverting that one change on the
current stack does restore `Tx Pos = 10`, `PACKET_LEN = 40` and the boot frame, so
those symptoms are confirmed to follow from it. `wlan0` does **not** return, so
"every downstream symptom" is established for the register-derived values and not
for the whole chain: at least one further difference stands between the restored
register path and a parsed boot event.

The change was **never committed.** It is an uncommitted local edit to the
vendored `esp-hosted` working tree, destroyed by a forced submodule checkout.

## 2. A0 execution record

### Step 1 — pre-A0 readback (restore point)

| | |
|---|---|
| File | `C:\Users\developer\tmp\easystick-p4-preflash-a0-20260812.bin` |
| Size | 16,777,216 bytes (full 16 MiB) |
| SHA-256 | `4d00ad7367a1f68318f18c2596059277e67d4cb7ab75096509726c4eeea710f7` |
| Taken | 2026-08-12 00:39:51 +0900, before any write |

### Step 2 — the preserved last-good set, re-verified

`C:\Users\developer\tmp\easystick-p4-m3-lab-resetpin-20260810\`, used exactly
as found. Nothing was rebuilt or regenerated.

| Offset | File | Size | SHA-256 |
|---|---|---:|---|
| `0x2000` | `bootloader.bin` | 22,976 | `681e730b7aa47eb2463cbb9fd48ece12c5f9df7e76b999cbeec5f3dc67b056de` |
| `0x8000` | `partition-table.bin` | 3,072 | `580a0ca942e6746a768724ae7bbb486dee47a228ca46b477ee598ac45738e87f` |
| `0x10000` | `boot-shim.bin` | 212,112 | `3e90e8b01bf2f347d6fd68c6fd3f67409b79c33fee2043773b46543ce44df418` |
| `0x90000` | `Image` | 6,497,284 | `6ee6e9844818f5fee8ed0fcf4ff1cf720caf0a99da7e443717eefad0fc59e53a` |
| `0x810000` | `rootfs.squashfs` | 1,826,816 | `a615f3c99a0d9c54c421399dfdc4806ce6790b32f740ce62730e583481849538` |
| `0xf10000` | `easystick-stamp-p4.dtb` | 2,858 | `a4284b333bfee614c6f41e7ed8566a910af7eca9916cff750a06c62fc439b0f7` |

The rootfs hash matches the stated known last-good value exactly.

### Steps 3–4 — flash, then a true cold boot

All six regions written and verified. The C6 was not touched. No kernel, rootfs,
DTB or boot-shim patching; no substitution from another candidate directory.

The power cycle was physical Type-C removal, and its completeness is recorded
rather than assumed. Two independent detectors were armed — COM10 absent from
`GetPortNames()` at 200 ms poll, and a `DEVPKEY_Device_LastArrivalDate` change —
and the presence-absence detector is the one that fired:

```
COM10 dropped at t=433.8s -- ABSENCE CONFIRMED; waiting for return
COM10 returned after 5.73s down; capture starting now
detector=absent open_attempts=1
```

5.73 s fully absent, so this is a cold boot and not a warm reboot. Because the
absence detector fired rather than the PnP fallback, the capture opened at port
return with the earlycon head intact. The collector was invoked **without**
`-Reset`, so nothing drove DTR/RTS, GPIO42 or any runtime reset sequence:
`reset_requested: false` in the manifest.

### Step 5 — capture

| | |
|---|---|
| Log | `C:\Users\developer\tmp\easystick-p4-a0-lastgood-restore-20260812.log` |
| Size | 73,870 bytes |
| SHA-256 | `01b702b5be51ccaed328296c63011cac28985fa3abf50ee639e121bac34fb55e` |
| Captured | 2026-08-11T15:51:27Z, COM10 @ 115200, 90 s window |

Build identity confirmed **by execution**, where it was previously known only
by hash (line 79):

```
Linux version 6.18.35 (root@ecd0a22cfb9a) (riscv32-buildroot-linux-uclibc-gcc.br_real
(Buildroot 2025.02.15) 13.4.0, GNU ld (GNU Binutils) 2.43.1) #29 Mon Aug 10 10:48:40 UTC 2026
```

## 3. Classification, earliest discriminator first

All six discriminators appear, in the order specified:

| # | Discriminator | A0 result | Failing runs | Line |
|---|---|---|---|---:|
| 1 | `get_firmware_data: Tx Pos` | **10** | 0 | 243 |
| 2 | `OPEN_DATA_PATH` | `ret=0` | `ret=0` | 245 |
| 3 | `BOOT_CMD53_RX: len=40` | **present** | absent | 249 |
| 4 | `Received ESP boot-up event` | **present** | absent | 255 |
| 5 | `ESP-Hosted Version` | `NG-1.0.6.0.1` | absent | 269 |
| 6 | `wlan0` appears | **`M3-lab: wlan0 appeared`** | never | 331 |

Discriminator 1 is the one that decides it, and it decided at line 243 — before
`OPEN_DATA_PATH`, before the ISR is registered, and before any line of patches
0009–0011 can run. Note that discriminator 2 is **not** discriminating: a write
that goes nowhere still returns 0, which is why `OPEN_DATA_PATH ret=0` was never
evidence of anything.

Supporting observations in the same capture:

- `SDIO IRQ received: read_ret=0 status=0x00800000` at 247 — the historical
  nonzero status, versus `0x00000000` in every failing run.
- `ESP SDIO probe completed` at 275; `Capabilities: 0xd` at 321.
- Live two-way traffic in the tail: `CMD53_TX: addr=0x1f600 bytes=512`,
  `BOOT_CMD53_RX: len=36`, and boot-frame data dumps. So the CMD53 **data path**
  works, which matters for §6.

### Fidelity: this is the historical state, including its limits

Compared against `easystick-p4-m3-cmd53regs-20260810.log`, the original #29 run:

| | Historical #29 | A0 restore |
|---|---|---|
| Build stamp | #29 | #29 |
| `Tx Pos` | 10 | 10 |
| `wlan0 appeared` | yes | yes |
| `Command[0x4]` timeouts | 6 | 4 |
| `Command[0xB]` timeouts | 8 | 6 |
| `Command[0xF]` timeouts | 12 | 14 |
| `Command[0xC]` timeouts | 0 | 2 |

A0 reproduced the historical secondary command timeouts too. That is the useful
part: it restored the historical state *with its known defects*, not an
idealised version of it. The `0xC` (association) timeouts appear only in A0
because its 90 s window reaches further into association than the original
capture did.

## 4. The rootfs bisect

Last-good `a615f3c9…` against first-bad `9f96804f…`. Byte-comparing the two
images is useless — squashfs compresses per block, so one changed file cascades
into 75.9% of subsequent bytes differing — so the comparison is per file, by
content hash, using `tools/squashfs-diff/`.

First, a fact that explains why the symptom never moved: the identical first-bad
rootfs `9f96804f…` is present in **six** later candidate directories
(`m3-lab-hosted-rebuild`, `m3-lab-live-ttygs1`, `m3-sdioagg`, `pio-candidate`,
`reset1500-candidate`, `diag-candidate-20260811-r1`). Every experiment after the
regression carried the same defective rootfs forward.

### Whole-image diff

| | |
|---|---|
| Inodes | 291 both sides |
| Only in A / only in B | **0 / 0** |
| Content or type changed | **2** |
| Metadata changed (mode/uid/gid) | **0** |
| `mtime` only | 46 |

| Path | Size | Hash |
|---|---|---|
| `/lib/modules/6.18.35/updates/esp32_sdio.ko` | 133,992 → 133,600 | `4ab4ecb0…` → `68d71a14…` |
| `/usr/sbin/dropbear` | unchanged | `14c862b2…` → `b966f7ad…` |

`dropbear` is the SSH daemon, same size, no SDIO involvement — consistent with
build nondeterminism, and not a candidate.

**This answers three of the four named areas definitively: init scripts,
module-loading behaviour and Buildroot package contents are byte-identical.**
Not "similar" — the same bytes, with zero added, removed, or re-permissioned
files. The 46 mtime changes are rebuild churn, reported separately so they
cannot bury the two that matter.

Corroboration from the images' own `mkfs_time`: last-good was packed
2026-08-10 10:48:50 UTC, **10 seconds** after build #29's kernel compile stamp
of 10:48:40 UTC — the same build run — and first-bad 8 m 06 s later.

### Inside `esp32_sdio.ko`

`srcversion` differs (`CEDE8AE6445B68211A17188` → `3D913CFF1557A476CE7BBE6`).
That hash covers the contributing sources, so this is a **source change**, not a
rebuild of the same source.

Comparing ELF32 RISC-V (`e_machine` 243) `FUNC` symbols in `.text` with nonzero
size, byte-for-byte:

| | Good | Bad |
|---|---:|---:|
| FUNC symbols in `.text` | 147 | 151 |
| Common | 147 | 147 |
| **Byte-identical** | **143 / 147** | |
| Differing | 4 | |
| Only in good | 0 | |
| Only in bad | 4 | |

Same compiler, same flags, 143 functions bit-for-bit identical. The entire delta
is eight symbols:

| Symbol | Good | Bad |
|---|---:|---:|
| `esp_read_reg` | 286 | 52 |
| `esp_write_reg` | 286 | 56 |
| `esp_read_block` | 306 | 48 |
| `esp_write_block` | 298 | 52 |
| `esp_read_byte` | — | 190 |
| `esp_read_multi_byte` | — | 154 |
| `esp_write_byte` | — | 174 |
| `esp_write_multi_byte` | — | 150 |

Printable strings: 726 on each side, differing **only** in the `srcversion`
line. So no diagnostic patch was added or dropped between the two builds — the
change is confined to these accessors.

### The mechanism, from relocations

`.rela.text` entries attributed to the enclosing `FUNC` symbol's address range
show which primitives each accessor actually calls, without needing a
disassembler:

| Function | Good build calls | Bad build calls |
|---|---|---|
| `esp_read_reg` | **`sdio_readb` only** | `esp_read_byte`, `esp_read_multi_byte` |
| `esp_write_reg` | **`sdio_writeb` only** | `esp_write_byte`, `esp_write_multi_byte` |
| `esp_read_block` | `sdio_memcpy_fromio`, `sdio_readb` | `esp_read_byte`, `esp_read_multi_byte` |
| `esp_write_block` | `sdio_memcpy_toio`, `sdio_writeb` | `esp_write_byte`, `esp_write_multi_byte` |

This is the finding. In the good build, `esp_read_reg` **has no
`sdio_memcpy_fromio` call site at all** — it cannot issue a CMD53, which is what
its 286 bytes are: a per-byte loop. Meanwhile `esp_read_block` kept *both*
primitives, i.e. upstream's size dispatch, merely inlined.

The bad build refactored both into shared helpers, and in doing so gave
`esp_read_reg` the size dispatch it never had. Every register access in
`esp_sdio.c` passes `sizeof(*val)` on a `u32 *` = 4 bytes, so the `size <= 1`
branch is never taken:

```c
/* esp_sdio.c:322 -- one of ten call sites, all 4-byte */
ret = esp_read_reg(context, ESP_SLAVE_TOKEN_RDATA, (u8 *) val,
                sizeof(*val), ACQUIRE_LOCK);
```

A 4-byte read of `TOKEN_RDATA` therefore went from 4× CMD52 to 1× CMD53. The
same applies to `INT_ST` (+0x58), `PACKET_LEN` (+0x60) and `INT_CLR` (+0xD4).

## 5. Provenance: why no commit contains the fix

| Check | Result |
|---|---|
| Commits touching the `esp-hosted` gitlink | exactly one, `ff6511a`, 2026-08-09 22:29 +0900 |
| That commit vs build #29 | precedes it (#29 built 08-10 19:48 JST) |
| Recorded gitlink SHA | `8626b42fd3f9eb5a1ccb5daea481f0d8d32b1685` |
| Actual submodule HEAD | identical |
| `esp_sdio_api.c` at `8626b42` | carries the **split** (bad) accessors |
| `esp_sdio_api.c` at clone tip `1df17f7` | carries the **split** accessors |
| Committed patches touching `esp_sdio_api.c` | none (before 0012) |

The pinned commit, the clone tip, and the entire committed patch series all
carry the CMD53-dispatching version. The byte-wise version existed only in the
submodule's **working tree**, uncommitted, and was destroyed by a forced
checkout — the same operation class root `CLAUDE.md` §14.22 records being run
across all five vendored submodules.

Nothing in Git records it. It is nonetheless exactly reconstructible, because
the module binary that contained it was preserved inside the last-good rootfs.

## 6. The reconstruction

`buildroot-external/package/esp-hosted-ng/0012-easystick-sdio-register-cmd52-access.patch`
— 2 hunks, +58/−8, 0 CR bytes, `patch -p1 --dry-run` reports `DRY-RUN OK`.

`esp_read_reg` and `esp_write_reg` become explicit byte loops under one
`sdio_claim_host`, so every register access is CMD52. Verified post-apply:

```
108:int esp_read_reg(...)        134:  data[i] = sdio_readb(func, reg + i, &ret);
154:int esp_write_reg(...)       180:  sdio_writeb(func, data[i], reg + i, &ret);
145:int esp_read_block(...)       77:  ret = sdio_memcpy_fromio(func, data, reg, size);
191:int esp_write_block(...)     100:  ret = sdio_memcpy_toio(func, reg, data, size);
```

The CMD53 **data path is deliberately untouched**: `esp_read_block` and
`esp_write_block` keep their size dispatch, so bulk RX/TX still uses
`sdio_memcpy_fromio`/`toio`. §3 shows that path working in the restored build,
so there is no reason to disturb it — and the split is exactly what the good
binary's relocations show.

This restores the shape the good build had. It does **not** explain why the C6
register window rejects a CMD53 multi-byte read on this host; the DW-MMC
controller, the `0x3FF` address mask, and the slave's own window decoding all
remain candidates. The patch avoids the transfer rather than explaining it, and
its header says so.

## 7. What this cannot see

- **A0 proves sufficiency, not exclusivity.** It shows the six P4 regions are
  sufficient to restore the behaviour. It does not prove no C6-side or external
  condition also contributes — only that none was *needed* on this power cycle.
  One clean cycle is not a repeatability measurement.
- ~~**The patch has not been executed.**~~ **Superseded by §9.8**, which is now
  classified: the A/B ran on hardware, byte-wise register access does restore
  `Tx Pos = 10` and the 40-byte frame on the current kernel, and `wlan0` remains
  absent. What §9.8 still cannot see is listed in §9.9, and the new open question
  it creates — a 40-byte frame whose payload repeats one 4-byte unit — is stated
  there rather than answered.
- **`dropbear` is unexplained.** Attributed to build nondeterminism on
  plausibility, not measured. It is not on any SDIO path.
- **Relocation attribution is not disassembly.** It proves a call site exists
  within a function's address range, not the order or condition of the calls.
  For this question that is sufficient — the good `esp_read_reg` has no CMD53
  call site to reach under *any* condition — but it would not settle a question
  about control flow.
- **`srcversion` proves a source change, not which change.** The
  identical-strings result bounds it to these accessors; it does not exclude a
  semantically inert edit elsewhere in the same file.
- **The C6 firmware was never in question and was never examined.** It was not
  reflashed, and its own boot path is not evidence here.

## 8. Defects found in the instruments while doing this

Recorded because both are the §14.2 shape — a check that was fine and simply
never saw the thing.

- **`grep` silently truncated a count on the A0 log.** Counting command
  timeouts returned `Command[0xF]` × 12 and no `Command[0xC]`; the log contains
  a byte that trips grep's binary heuristic partway through, and everything past
  it was dropped with only a `Binary file (standard input) matches` line to say
  so. Re-run with `-a`: `0xF` × 14 and `0xC` × 2. Any count taken off these
  captures needs `-a`, and a count that arrives *smaller* than expected is the
  symptom to distrust.
- **A negative-test harness graded the wrong column.** The content-corruption
  control for `squashfs-diff` extracted the `uid:gid` field instead of the
  `sha256` field, so it compared identical strings and reported "0 differing
  files" — making a working tool look blind. Fixed, and the mutated offsets were
  then confirmed to lie inside live byte ranges before being trusted, rather
  than assumed to.
- **The capture collector lost the one boot it was armed for.** The cold-boot
  capture in §9.8 returned **0 bytes** while the board was in fact printing a
  full boot log, which a later fresh open picked up mid-kernel. Cause: the
  ESP32-P4 USB-Serial/JTAG device re-enumerates when the running image
  re-initializes it, so the handle `capture-boot.ps1` opened the moment COM10
  returned was already stale — and a collector that opens **once** cannot tell a
  stale handle from a silent target. Both look like zero bytes, and zero bytes
  reads as a finding. Fixed by reopening until the first byte arrives and
  recording `port_opens` in the manifest, so "it took N opens" is evidence
  instead of an invisible retry. This is the §14.19 shape rather than plain
  §14.2: the instrument did not measure the wrong value, it answered a different
  question — "did this handle carry bytes" — while reporting an answer to "was
  the target silent".
- **The fix's own negative test caught a defect in the fix.** Deliberately
  running the repaired collector against a silent port failed with
  `SyntaxError: unterminated string literal` — the Windows native argument
  marshaller had stripped the quotes out of `b"".join(chunks)` in the `-c`
  payload, leaving `b".join(chunks)`. The file's own comment warns about
  embedded quotes and the new code had reintroduced one anyway; rewritten as
  `bytes().join(chunks)`. Worth recording because the defect was in the two
  lines added to repair an instrument, and only the negative test ran them —
  §14.2, one level in.

Separately, and unrelated to this investigation: `vendor/linux` reports 13
permanently-modified files. They are **not** CRLF corruption. All 13 have
exactly two case-variant twins in Git's index (`xt_MARK.h` / `xt_mark.h`,
`xt_DSCP.c` / `xt_dscp.c`, and so on), so on this case-insensitive filesystem
only one can exist and Git reports the survivor as modified at the other path.
None of the affected netfilter targets is enabled in `m2/kernel.config.fragment`,
so it is benign permanent dirt rather than a build hazard — but `git status`
inside that submodule will never be clean on Windows, and that should not be
read as corruption returning.

## 9. The 0012 A/B on the current stack

The A/B §7 called the natural next step. One intentional functional change —
`0012-easystick-sdio-register-cmd52-access.patch` — built into the **current**
source and patch series, with the 0010/0011 diagnostics and the current resetpin
policy left exactly as they are. The C6 was not modified, reflashed or probed,
and no second patch was stacked on 0012.

Status: **built, flashed and verified on the P4; hardware result not yet
captured.** The board is parked in the ROM bootloader (`--after no_reset`) and
has not yet booted this image. Nothing below is a hardware claim.

### 9.1 Build

`build-m1.sh /src /out --profile m2` in the pinned container, exit 0.
`M2 Buildroot build complete: /out/buildroot/images`.
Log `C:\Users\developer\tmp\easystick-p4-cmd52-build-20260812.log`,
29,737 bytes, SHA-256 `05550c94462276a67cba536e80d0db7bee6461202b6185ec1d5d67a247c544c1`.

The baseline it is measured against is the israck candidate
(`easystick-p4-israck-candidate-20260811`), which is the same `--profile m2`
build of the same series **without** 0012 — established by its rootfs matching
byte for byte and by reading `/out/buildroot/.config`, not by trusting the
directory name.

### 9.2 The series applies cleanly, including 0012

Eleven patches, in order, after `esp-hosted-ng-dirclean` discarded the package:

```
0001-linux-6.18-netdev            0007-easystick-wifi-only-host-module
0002-easystick-stamp-p4-sdio-defaults   0008-easystick-first-cmd53-tx-diagnostics
0003-easystick-sdio-handshake-diagnostics  0009-easystick-sdio-rx-aggregate
0005-easystick-sdio-drain-boot-packet  0010-easystick-sdio-boot-packet-len-poll
0006-easystick-sdio-cmd53-diagnostics  0011-easystick-sdio-isr-acknowledge
                                      0012-easystick-sdio-register-cmd52-access
```

Zero occurrences of `fuzz`, `FAILED`, `Reversed` or `malformed` in the whole log.
Several hunks applied at a line offset, which is normal and is not fuzz. 0004 is
absent by design, not skipped by accident — the `.mk` applies a `*.patch` glob
with no series file, so a present file cannot be silently ignored.

### 9.3 The built module's register path, measured from the binary

Measured with the relocation attributor, **calibrated in both directions first**
against two modules whose answers are already known (§14.2: an instrument that
has only ever agreed with you has not been tested):

| Module | SHA-256 | `esp_read_reg` | reaches | byte helpers |
|---|---|---:|---|---|
| last-good #29 | `4ab4ecb0…` | 286 | `sdio_readb` | absent |
| israck (first-bad) | `758f29f6…` | 52 | *nothing* — dispatches | present |
| **0012 candidate** | `4ca296b8…` | **262** | **`sdio_readb`** | absent |

The 0012 module: `srcversion 74FADD67DCAE5A259C9C7CC`, `vermagic 6.18.35 riscv`.

```
esp_read_reg     size=262  ['sdio_claim_host', 'sdio_readb', 'sdio_release_host']
esp_write_reg    size=260  ['sdio_claim_host', 'sdio_release_host', 'sdio_writeb']
esp_read_block   size=306  ['sdio_claim_host', 'sdio_memcpy_fromio', 'sdio_readb', 'sdio_release_host']
esp_write_block  size=298  ['sdio_claim_host', 'sdio_memcpy_toio', 'sdio_release_host', 'sdio_writeb']
esp_read_byte, esp_read_multi_byte   ABSENT
```

So `esp_read_reg` has a `sdio_readb` call site and **no** `sdio_memcpy_fromio`
call site — there is no CMD53 path for it to reach under any condition. The four
byte helpers are absent because each became single-use and GCC inlined it;
absence here means inlined, not deleted, which is why the call sites moved into
the enclosing accessors rather than disappearing.

### 9.4 The block/data path is unchanged

Two diffs, because the two comparisons answer different questions.

Against the **current stack** (israck → 0012), the functional delta is exactly
the intended one:

```
funcs 152 -> 148, common 148, byte-identical 144, changed 4
CHANGED esp_read_reg 52->262   esp_write_reg 56->260
CHANGED esp_read_block 48->306 esp_write_block 52->298
only in israck: esp_read_byte, esp_read_multi_byte, esp_write_byte, esp_write_multi_byte
```

The two block functions change **size** here only because the israck versions
were 48/52-byte dispatch stubs into the helpers; the transfer primitive they
reach is unchanged, and the second diff proves that directly.

Against **#29** (last-good → 0012), `esp_read_block` (306) and `esp_write_block`
(298) are **byte-identical** — they do not appear in the changed list at all:

```
funcs 147 -> 148, common 147, byte-identical 138, changed 9
CHANGED esp_read_reg 286->262      esp_write_reg 286->260
CHANGED esp_handle_isr 274->502    esp_probe 820->994    esp_remove 298->318
CHANGED read_packet 1064->1204     esp_init_interface_layer, get_firmware_data, tx_process
only in 0012: esp_get_len_from_slave
```

The seven non-accessor differences and the one new function are the 0010/0011
diagnostics, which #29 predates and which this experiment deliberately keeps.
The two register accessors differ in size from #29 because 0012 is a
**reconstruction** of that shape, not a byte recovery of it — the good bytes
were never committed (§5), only their relocations survive.

Which patch prints which marker, attributed from the patch files rather than
from the marker names, because two of them mislead:

| Marker | Patch | In #29? |
|---|---|---|
| `OPEN_DATA_PATH write completed`, `ESP SDIO probe completed` | 0003 | yes |
| `SDIO IRQ received: read_ret=… status=…` | 0003 | yes |
| `BOOT_CMD53_RX: len=…`, `BOOT_CMD53_DATA:` hexdump | **0006** | **yes** |
| `BOOT_POLL: iter=… len=… ret=…` | **0010** | no |
| `SDIO IRQ received: status=… dispatch=… after_empty=…` | 0011 | no (replaces 0003's line) |

The middle row is what makes §9.8's payload comparison possible at all: the
40-byte frame dump is **0006**, which both sides carry, so #29 and the candidate
print the same diagnostic in the same format. Its presence in #29 is *not*
evidence that #29 carries 0010 — a mistake made here once, on exactly that
inference, and corrected against these patch files. `BOOT_POLL` is 0010's marker
and appears 0 times in A0; `after_empty` is 0011's and also appears 0 times.
So #29 lacks both, as stated above.

### 9.5 Flashed artifacts

`C:\Users\developer\tmp\easystick-p4-cmd52-candidate-20260812\`

| Offset | File | Size | SHA-256 |
|---|---|---:|---|
| `0x2000` | `bootloader.bin` | 22,976 | `39a09d946c6425daa7827f581d8f03ea5b3d335156d37dc3c53c0ae1c584b912` |
| `0x8000` | `partition-table.bin` | 3,072 | `580a0ca942e6746a768724ae7bbb486dee47a228ca46b477ee598ac45738e87f` |
| `0x10000` | `boot-shim.bin` | 212,112 | `9d1a25bd9b65df26681fdc782bb230aecb48895ada2de77cef2cdbc48855c353` |
| `0x90000` | `Image` | 6,568,764 | `d52bb98aa60089cbff9c3c67345df2a4903e9c73e46f1d883663efc82b43d57d` |
| `0x810000` | `rootfs.squashfs` | 1,822,720 | `e26c2c9a2e8276c3a2e569a4ca9e113559405a417e9f88a6ed5448d0e6a85123` |
| `0xf10000` | `easystick-stamp-p4.dtb` | 3,084 | `0fb1f66abdc4dfd19d14c7543cb6acddeb99ee72f430f42cfce9218e7c19528d` |

`bootloader.bin`, `partition-table.bin`, `boot-shim.bin` and the DTB are the
israck files unchanged (the DTB byte-identical). `Image` has a different hash at
identical size, so the delta was measured rather than assumed: **68 differing
bytes in 6 runs, none of it code** — the Docker container hostname
`root@f48e18f49ea8` → `root@21ee02b4fa16` in three version strings, the build
stamp `#5 Tue Aug 11 09:11:52 UTC 2026` → `#6 Wed Aug 12 00:16:04 UTC 2026`
twice, and the GNU build-id note at `0x5239f0`. Kernel `#N` resets when the tree
is cleaned, so the stamp is the identifying key, not the number.

The rootfs delta is **exactly one file**: 289 inodes on both sides, 0 added,
0 removed, 0 metadata changes, and one content change —
`/lib/modules/6.18.35/updates/esp32_sdio.ko`, `758f29f6…` → `4ca296b8…`,
135,804 → 136,204 bytes. `dropbear` is unchanged, which retires §7's open
`dropbear` question for this build: its earlier change was `dropbear-dirclean`
collateral, not nondeterminism in the SDIO path.

### 9.6 Recovery set, re-verified before the write

All six preserved #29 regions in
`C:\Users\developer\tmp\easystick-p4-m3-lab-resetpin-20260810\` match their §2
hashes exactly (rootfs `a615f3c99a0d…`), and
`easystick-p4-preflash-a0-20260812.bin` is intact at 16,777,216 bytes /
`4d00ad7367a1…`.

One planned extra restore point was **not** obtained. A fresh 16 MiB readback of
the then-current (restored, working) flash failed twice: `Corrupt data, expected
0x1000 bytes but received 0xb6c bytes` at 84 % on 460800 baud, then 12 % in ten
minutes at 230400. The partial file was deleted rather than kept, because a
truncated image that looks like a restore point is worse than no restore point.
Recovery was unaffected: the pre-0012 on-device state was exactly the six
preserved regions written over `…preflash-a0…`, both of which are on disk, so it
remains byte-reconstructible. `read_flash` is read-only and the target was not
modified by either attempt.

### 9.7 Write and verify

`flash-candidate.ps1 -AllowCandidateWrite`, gate output
`C6 write: none (COM10 is the P4 module USB-C path)`; stock-readback gate
satisfied by `easystick-p4-stock-20260809-esptool481-full-v2.bin`
(`229459f2…`, 16 MiB). All six regions `Hash of data verified` on write, and an
independent `verify_flash` pass reports `-- verify OK (digest matched)` for the
partition table, boot shim, `Image`, rootfs and DTB; `bootloader.bin` is covered
by its write-side hash because esptool rewrites its header byte and digest while
programming. Log
`C:\Users\developer\tmp\easystick-p4-cmd52-flash-20260812.log`.

### 9.8 Cold boot and capture

The cold-boot gate is read-only: it never writes the target and invokes
`capture-boot.ps1` **without** `-Reset`, so nothing drives DTR/RTS or GPIO42.
Two independent detectors as in §"Steps 3–4" — COM10 leaving `GetPortNames()` at
a 200 ms poll (primary) and a `DEVPKEY_Device_LastArrivalDate` change (throttled
fallback, because `Get-PnpDevice` costs about a second and must not slow the
absence poll).

A first 15-minute arm window expired with no detach and captured nothing; that
is recorded rather than retried silently, because "no capture" and "a capture
that shows nothing" are different results. On the second arm the cycle was
performed and the primary detector confirmed it: `COM drop at t=371.9s --
ABSENCE CONFIRMED`, then `COM10 returned after 4.72s down`, `detector=absent`.
**That capture returned 0 bytes** (`e3b0c442…`, the empty-input hash) because of
an instrument defect diagnosed and fixed afterwards — §8.

The classified capture is therefore a **later boot on the same flashed image**,
not the instant of the cold cycle: `easystick-p4-cmd52-boot-20260812.log`,
10,174 bytes, `6731768a50f7bf450a76aea3053a111c00847435ab02bb28d5bc9fa1d8b78464`,
`reset_requested: false`, `port_opens: 1`. It contains exactly one boot — one
`legacy bootconsole … enabled`, one `keep_bootcon` cmdline, one real
`Run /sbin/init` (printed twice by the earlycon+console doubling that every
capture on this stack carries; halve all counts below). Its first line is a
partial `MIO32 0x500d2000`, so a few bytes of the first earlycon line were lost
before the handle opened; everything from bootconsole registration onward is
present.

#### Classification, earliest discriminator first

| # | Discriminator | Result | Measured |
|---:|---|---|---|
| 1 | raw/derived token state, `Tx Pos` | **PASS** | `Rx Pre 0`, `Rx Pos 0`, `Tx Pre 0`, **`Tx Pos 10`** |
| 2 | `OPEN_DATA_PATH` | **PASS** | `write completed: ret=0` |
| 3 | `BOOT_CMD53_RX: len=40` | **PASS** | `BOOT_POLL: iter=0 elapsed_ms=0 len=40 ret=0`, then `read_packet: BOOT_CMD53_RX: len=40 ret=0` |
| 4 | `Received ESP boot-up event` | **FAIL** | 0 occurrences |
| 5 | `ESP-Hosted Version` | **FAIL** | 0 occurrences |
| 6 | `wlan0` | **FAIL** | `M3-lab: no wlan0; SSH remains disabled` |

**The critical expected transition happened.** `Tx Pos = 0` → with 0012 →
`Tx Pos = 10`, and all four token values now match the last-good #29 boot
*exactly* — `0 / 0 / 0 / 10` at A0 log lines 237–244. So the branch that would
have required stopping and reporting relocation evidence did not fire.
`PACKET_LEN` is fixed too: the length register that read 0 on every failing run
now reads 40, the same value #29 read.

The chain nevertheless stops before `wlan0`, so **the root cause is confirmed for
the register path and is not confirmed end to end.**

#### Where it diverges from #29: the payload, not the length

Both modules carry **0006**, which is the patch that prints the frame dump, so
the bytes are directly comparable — that is the only diagnostic the two sides
share here, and it is why this comparison is possible at all. #29 carries 0003 +
0006 and neither 0010 nor 0011 (§9.4). 0006 dumps `min(len_from_slave, 32)` bytes
at 16 per line, so each event is two lines and both sides show the first 32 of
the 40:

```
#29  (wlan0 OK)   03 00 00 00 1c 00 0c 00 00 00 00 00 01 00 15 00
                  14 00 00 00 03 01 0d 00 01 0d 01 0c 4e 47 00 01
0012 candidate    03 00 00 00 03 00 00 00 03 00 00 00 03 00 00 00
                  03 00 00 00 03 00 00 00 03 00 00 00 03 00 00 00
```

The **first four bytes agree**. After that #29 streams a real header — `1c 00` =
28-byte payload, `0c 00` = offset 12, and `4e 47` = ASCII `NG`, the first two
characters of the version string it goes on to print — while the candidate
repeats the same four bytes for every logged byte. The driver reads that as
`len=3 offset=0` and drops it: `Drop invalid pkt: len=3 offset=0`, then
`BOOT_CMD53_RX: len=0`.

This is not one unlucky frame. Across the **whole** capture there is exactly
**one distinct** `BOOT_CMD53_DATA` line, the replicated one; #29's capture has
**67 distinct** payload lines. Every CMD53 block read logged on the candidate
returns a 4-byte unit repeated across the buffer.

`esp_probe … failed with error -22` is **not** a discriminator: A0 log line 279
shows #29 producing the identical `-22` and then creating `wlan0` anyway. It was
present in the working case and must not be read as this run's fault.

#### The residual delta, and what it does to the A/B

§9.9's fourth bullet pre-registered that this A/B holds the current diagnostics
fixed and therefore could not show them inert. That limitation is now load-
bearing, because the two sides differ by two diagnostics as well as by 0012:

| | #29 last-good | 0012 candidate |
|---|---|---|
| `BOOT_POLL` (**0010**) | **0** occurrences | present, `iter=0 elapsed_ms=0 len=40` |
| ISR line form (**0011**) | `read_ret=0 status=0x…` | `status=0x… dispatch=1 after_empty=0` |
| `esp_get_len_from_slave` | absent from module | present |
| `BOOT_CMD53_DATA` (0006) | present | present |

The consequence for this experiment is specific, and it is 0010's. That patch
replaces 0005's single `msleep(100)` drain with `context->rx_byte_count = 0`,
then up to ten `esp_get_len_from_slave()` reads at 300 ms spacing, then a manual
`esp_process_new_packet_intr()` — all inside `esp_probe`, before the ISR-driven
read. A probe-time length read plus a zeroed producer count plus a manual receive
is a **candidate confound** for a frame that arrives at the right length with the
wrong payload, and it is a difference #29 does not have. It is measured only as a
difference between the two sides; nothing here shows it is the cause, and
`BOOT_POLL` stays in place until the ordered experiments say otherwise.

Per the standing instruction, **no second patch was stacked on 0012** and no
driver change was made in response to this result.

### 9.9 What this step cannot see, stated before the result

- **It is one power cycle, not a repeatability measurement.** A0 had the same
  limit and it is worth restating: one clean cycle each way is an A/B, not a
  distribution.
- **A relocation is not an execution.** §9.3 proves `esp_read_reg` cannot reach
  CMD53 in this binary. It does not prove the register read succeeds, only that
  the transfer form is the one #29 used.
- **`Image` byte-identity was not required and was not achieved.** The six
  differing runs are a hostname, a timestamp and a build id; that is a claim
  about *those* bytes, established by comparison, and not a general claim that
  the two kernels are functionally identical.
- **The A/B holds the current diagnostics fixed on purpose.** So a positive
  result would confirm the register-access mechanism on the current stack; it
  would not show that 0010/0011 are inert, and it would not explain *why* the C6
  register window rejects a CMD53 multi-byte read (§6 — the patch avoids the
  transfer rather than explaining it).
- **The C6 remains unexamined.** Not reflashed, not probed, not wired.

Added **after** the result, and marked as such so the pre-registered list above
stays readable as what was known in advance:

- **The classified boot is not the cold-cycle boot.** The cold cycle happened and
  was detected (4.72 s absent), but its capture returned 0 bytes; §9.8's
  classification comes from a later boot of the same flashed image. Nothing drove
  DTR/RTS or GPIO42 in either (`reset_requested: false`), and no rail state was
  measured for the classified boot. A cold cycle is still the discipline this
  experiment asked for, and one more would make the classification match it.
- **The replicated payload is not attributed.** `esp_read_block` is
  byte-identical to #29's (§9.4) and still returns a repeated 4-byte unit here,
  so the difference is not in that code. Two candidates are open and this capture
  separates neither: the C6 genuinely placed those bytes in the buffer, or the
  transfer read a 4-byte-wide source without advancing. **0010's** probe-time
  read is a third, and it is a measured *difference*, not a measured cause.
- **A repeating byte pattern is a weak fingerprint.** `03 00 00 00` is also the
  correct first four bytes of the real frame, so "the first read was right and
  then repeated" and "the whole buffer happens to contain this" are consistent
  with the same 32 logged bytes. The last 8 bytes of the 40 are not logged at
  all by either module.

## 10. The strict cold-boot repeat, under the required discipline

§9.9's first post-result bullet recorded that the classified 0012 boot was *not*
the cold-cycle boot: the cycle happened and was detected, but its capture
returned 0 bytes and the classification came from a later boot of the same image.
That gap is now closed. The capture helper was repaired first (§8), then one
genuine Type-C detach/reconnect was performed with the **flashed 0012 candidate
unchanged and no flash write of any kind**.

`C:\Users\developer\tmp\easystick-p4-coldboot-0012-repeat-20260812.log`
11,169 bytes, `7a2e86a99837b3bae79f313984f19e1adf43c2721d1df46e34440894d0c7a398`

| Recorded quantity | Value |
|---|---|
| Detach detector | `absent` — COM10 left `GetPortNames()` and returned |
| Absence interval | **4.94 s** down |
| `port_opens` | 27 |
| `first_byte_seconds` | 2.014 |
| `control_lines_driven` | **False** — no DTR/RTS, no GPIO42, no runtime reset |
| Build stamp | `6.18.35 (root@21ee02b4fa16) #6 Wed Aug 12 00:16:04 UTC 2026` |
| Transfer mode | `dw_mmc 50083000.mmc: Using PIO mode.` |
| Boots in window | 1; `keep_bootcon` and `legacy bootconsole` are both present, so the capture starts before the console handoff rather than joining after it |

The build stamp is the identifying key, not the `#N` counter (§9.5). It matches
the flashed 0012 candidate exactly, so this capture provably ran that image and
not a neighbour.

### 10.1 Classification, earliest discriminator first

| # | Discriminator | Result | Evidence |
|---|---|---|---|
| 1 | token state | **PASS** | `Rx Pre 0`, `Rx Pos 0`, `Tx Pre 0`, `Tx Pos 10` — matches #29 |
| 2 | `OPEN_DATA_PATH` | **PASS** | `write completed: ret=0` |
| 3 | first `PACKET_LEN` | **PASS, = 40** | `BOOT_POLL: iter=0 elapsed_ms=0 len=40 ret=0`, then `BOOT_CMD53_RX: len=40 ret=0` |
| 4 | `Received ESP boot-up event` | **FAIL** | 0 occurrences |
| 5 | `ESP-Hosted Version` | **FAIL** | 0 occurrences |
| 6 | `wlan0` | **FAIL** | `M3-lab: no wlan0`, `wlan0 did not appear during first-boot DHCP window` |

### 10.2 The payload, which is the decisive comparison

`len=40` is not the discriminator — #29 also read 40. The payload is:

```
#29  (wlan0 OK)   03 00 00 00 1c 00 0c 00 00 00 00 00 01 00 15 00
                  14 00 00 00 03 01 0d 00 01 0d 01 0c 4e 47 00 01
cold repeat       03 00 00 00 03 00 00 00 03 00 00 00 03 00 00 00
                  03 00 00 00 03 00 00 00 03 00 00 00 03 00 00 00
```

Counted rather than eyeballed: 0006 dumps `min(len, 32)` bytes at 16 per line, so
one event is two lines, and console line doubling makes four. **All four lines in
this capture are byte-identical**, so bytes 0-31 are `03 00 00 00` repeated eight
times. Bytes 32-39 are not dumped by either module. Then
`Drop invalid pkt: len=3 offset=0` and `BOOT_CMD53_RX: len=0`.

So the historical frame is **not** restored by cold-boot discipline. The payload
corruption reproduces under the strict protocol, which retires the possibility
that §9.8's result was an artifact of classifying a warm boot. The condition for
proceeding to the internal-DMA A/B is met.

## 11. Instrument defect: the FORCE_PIO build switch did nothing

Found before flashing, by measuring the build product instead of trusting the
build's exit status.

`build-m1.sh` selects the DMA arm by **omitting**
`0008-easystick-dw-mmc-force-pio.patch` from the list it stages into
`BR2_LINUX_KERNEL_PATCH`. Buildroot applies that list only when it *extracts* the
kernel, and the tree was already extracted and patched by the previous PIO build.
So `EASYSTICK_SDIO_FORCE_PIO=0` completed successfully, reported
`M2 Buildroot build complete`, and produced a **PIO kernel**:

```
>>> linux custom Updating kernel config with fixups     <- no "Patching" step
/out/buildroot/build/linux-custom/drivers/mmc/host/dw_mmc.c:3138:
        if (device_property_present(dev, "easystick,force-pio"))
grep -c "easystick,force-pio" /out/buildroot/images/Image  ->  1
```

Had that been flashed it would have compared PIO against PIO while labelling
itself DMA — and the label would have come from the build script's environment
variable, which is the one thing in the chain that cannot see whether the patch
was applied. Recovery is `rm -rf /out/buildroot/build/linux-custom`, which drops
the stamps and forces a re-extract.

- A build switch that selects a patch **set** is only honoured on a clean
  extract. **MUST NOT** treat a successful incremental build as evidence that the
  switch took effect; assert the switch's *effect* in the build product.
- The assertion is cheap and belongs in the tool: the string
  `easystick,force-pio` must appear in the built `Image` when
  `EASYSTICK_SDIO_FORCE_PIO=1` and must be absent when `0`. That is a check
  against the artifact, not against the variable that requested it (root
  CLAUDE.md §14.16: a check that reads its threshold from the same place as the
  thing under test proves only self-consistency).
- This is §14.2 at the build layer. Nothing was red: exit 0, a plausible log, and
  a complete artifact set. The defect was invisible in every signal except the one
  nobody had looked at.

### 11.1 The clean rebuild, proven to have changed only the intended variable

| Check | Result |
|---|---|
| Kernel `Extracting` + `Patching` steps ran | yes; both absent in the bad build |
| `easystick,force-pio` in extracted kernel source | **0** (was 1) |
| `easystick,force-pio` in built `Image` | **0** (was 1) |
| Kernel patch list delta vs PIO build | exactly one entry removed, 0008; 15 others identical |
| esp-hosted-ng patches | all 11 applied, 0010/0011/0012 included, unchanged |
| `esp32_sdio.ko` | `4ca296b8…` — **byte-identical** to the flashed 0012 module |
| `easystick-stamp-p4.dtb` | `0fb1f66a…` — **byte-identical** to the flashed DTB |
| `rootfs.squashfs` content | 289 inodes both sides, **zero** differing files |
| `Image` | `9455e686…` (was `d52bb98a…`), same size 6,568,764 |

The DTS still contains `easystick,force-pio`; with 0008 absent the property is
simply unread, which is why the DTB is byte-identical and why no DTS edit was
needed. The `Image` delta was measured, not assumed: 64,837 differing bytes in
10,305 runs, of which 2,202 two-byte runs carry an identical `+320` little-endian
delta — a uniform address shift from a 320-byte text shrink — plus localized
codegen change concentrated at `0x2263f2`, the `dw_mci_init_dma` region. That is
the shape of a code removal, not of drift.

Flashed artifacts, P4 only, C6 untouched:

| Offset | File | SHA-256 | vs 0012 candidate |
|---|---|---|---|
| `0x2000` | `bootloader.bin` | `39a09d94…` | same |
| `0x8000` | `partition-table.bin` | `580a0ca9…` | same |
| `0x10000` | `boot-shim.bin` | `9d1a25bd…` | same |
| `0x90000` | `Image` | `9455e686…` | **changed** |
| `0x810000` | `rootfs.squashfs` | `e26c2c9a…` | same file, byte-for-byte |
| `0xf10000` | `easystick-stamp-p4.dtb` | `0fb1f66a…` | same |

All five non-bootloader regions returned `verify OK (digest matched)`; the
bootloader passed its write-side `Hash of data verified`. Recovery set intact and
unchanged: stock `229459f2…`, pre-A0 `4d00ad73…`, israck `8eb22bff…`,
wlan0-repro `e86fe758…`. The 0012 candidate remains staged on the host at
`easystick-p4-cmd52-candidate-20260812\`, so the PIO arm can be restored by
reflashing one region.

**The runtime proof of the DMA arm is `Using internal DMA controller` in the boot
log.** This step MUST NOT be classified as the DMA arm without that line — the
PIO baseline in §10 prints `Using PIO mode.` in the same position, so the two arms
are distinguishable at runtime and neither is taken on faith from the build.

## 12. Strict cold DMA result: boot frame restored, post-boot response corrupted

The clean DMA candidate from §11.1 was captured after a genuine Type-C detach
and reconnect. No additional build, flash write, source change or C6 action was
performed between §11.1's verified P4 flash and this capture.

The first operator cycle was deliberately rejected before the collector opened:
COM10 was absent for only 4.07 s, below the pre-registered 4.5 s gate. It produced
no capture file and is not classified. The accepted cycle then held COM10 absent
for **9.42 s** and started the passive 90 s collector only after the port returned.

`C:\Users\developer\tmp\easystick-p4-coldboot-dma-20260812.log`
17,116 bytes,
`99a1379bfa656abcb507859529935484b44c15b2d92b7b4fbe73d741c81c9e8d`

| Recorded quantity | Value |
|---|---|
| Detach detector | `absent` — COM10 left `GetPortNames()` and returned |
| Absence interval | **9.42 s** down |
| `port_opens` | 19 |
| `first_byte_seconds` | 2.004 |
| `control_lines_driven` | **False** — `reset_requested: false`; no DTR/RTS, GPIO42 or runtime reset |
| Build stamp | `6.18.35 (root@76a3c84d5ea1) #1 Wed Aug 12 02:23:04 UTC 2026` |
| Transfer mode | **`Using internal DMA controller`**; zero `Using PIO mode` occurrences |
| Boots in window | 1; one `keep_bootcon` and one `legacy bootconsole` marker |

### 12.1 The pre-registered boot discriminators

| # | Discriminator | Result | Evidence |
|---:|---|---|---|
| 1 | token state | **PASS** | `Rx Pre 0`, `Rx Pos 0`, `Tx Pre 0`, `Tx Pos 10` |
| 2 | `OPEN_DATA_PATH` | **PASS** | `write completed: ret=0` |
| 3 | first `PACKET_LEN` | **PASS, = 40** | `BOOT_POLL: iter=0 elapsed_ms=0 len=40 ret=0`, then `BOOT_CMD53_RX: len=40 ret=0` |
| 4 | first 40 payload bytes | **PASS for the 32 logged bytes** | exact #29 frame prefix; as before, 0006 does not dump bytes 32–39 |
| 5 | `Received ESP boot-up event` | **PASS** | present |
| 6 | `ESP-Hosted Version` | **PASS** | `NG-1.0.6.0.1` |
| 7 | `wlan0` | **FAIL** | `M3-lab: no wlan0`, followed by the first-boot DHCP-window failure |

The first DMA receive is the exact historical payload rather than the PIO
repeated-word result:

```
#29 / DMA candidate  03 00 00 00 1c 00 0c 00 00 00 00 00 01 00 15 00
                     14 00 00 00 03 01 0d 00 01 0d 01 0c 4e 47 00 01
PIO cold repeat      03 00 00 00 03 00 00 00 03 00 00 00 03 00 00 00
                     03 00 00 00 03 00 00 00 03 00 00 00 03 00 00 00
```

That establishes a transfer-mode-dependent P4 host regression for the first
bulk CMD53 receive: keeping the DTB, rootfs/module, 0010/0011/0012, C6, reset
waveform, bus width and 400 kHz request fixed, removing forced PIO changes the
bad frame back to the historical frame. It does **not** make internal DMA an
end-to-end workaround, because the run still fails before `wlan0`.

### 12.2 The new boundary is the first command response

After the boot event and version, the host transmits the 512-byte interface-init
command. The next real slave interrupt reports `status=0x00800000`; the length
register reads 32, but the DMA destination contains:

```
6d 63 30 3a 30 30 30 31 3a 31 2f 69 65 65 65 38
30 32 31 31 2f 70 68 79 30 00 41 43 54 49 4f 4e
```

That is ASCII `mc0:0001:1/ieee80211/phy0\0ACTION`, not an ESP-Hosted frame. It
is parsed as `len=12336 offset=12592`, dropped, and command `0x1` times out;
`esp_add_network_ifaces()` then fails. In #29 the equivalent 32-byte response
was instead a valid ESP-Hosted command frame:

```
00 00 02 00 14 00 0c 00 00 00 00 00 01 02 00 00
00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00
```

The ASCII has the shape of host-side wiphy/uevent memory and appears precisely
while the host is registering `phy0`. That makes stale or incorrectly
synchronised DMA destination memory the leading interpretation, but it is an
**inference**, not a proved cause. The capture alone cannot distinguish that
from the C6 returning those bytes. It does prove that the first DMA transfer can
be correct while a later transfer on the same boot is not.

0011's zero-status fallback is not implicated by this run: both relevant IRQs
carry `status=0x00800000`, both say `after_empty=0`, and no empty-IRQ path is
logged. Do not isolate 0011 on this evidence.

### 12.3 Classification and next controlled step

This is a **partial DMA success**, a result not covered by the two original
binary branches:

- it is not the DMA-failure branch, because the repeated `03 00 00 00` boot
  payload did not persist;
- it is not the end-to-end DMA-success branch, because `wlan0` did not return.

No further patch was stacked. The next smallest measurement is one unchanged
strict-cold repeat of this same DMA image, to determine whether the correct
first frame plus corrupt 32-byte command response is reproducible. If it is,
the next source-level A/B should keep DMA, 0011 and 0012 fixed and remove only
0010's bounded probe-time poll, with a clean rebuild and the same artifact gates
used in §11.1. Be precise about that A/B: 0010 changes the poll/timing, while
0005 introduced the manual `esp_process_new_packet_intr()` drain. Removing 0010
alone therefore does not remove the manual receive path.

## 13. Unchanged DMA repeat: the first-response corruption is reproducible

The exact candidate flashed for §12 was run again without a rebuild, flash
write, source change, C6 action, patch change or software reset. The external
detector saw COM10 detach at `2026-08-12T03:44:45.4835684Z`, return at
`2026-08-12T03:44:50.6768250Z`, and therefore remain absent for **5.193 s**.
Only after the return did the passive 90 s collector open the port.

`C:\Users\developer\tmp\easystick-p4-coldboot-dma-repeat2-20260812.log`
15,991 bytes,
`66ca6d059f99f7d71fc15a8a015602cb245f9a7f558e9f1a4de7d7b503eb866d`

| Recorded quantity | Value |
|---|---|
| `port_opens` | 19 |
| `first_byte_seconds` | 2.014 |
| `control_lines_driven` | **False**; `reset_requested: false` |
| Build stamp | `6.18.35 (root@76a3c84d5ea1) #1 Wed Aug 12 02:23:04 UTC 2026` |
| Transfer mode | **`Using internal DMA controller`** |

The requested checkpoints, in their pre-registered order, were:

| # | Checkpoint | Unchanged-repeat result |
|---:|---|---|
| 1 | `Tx Pos` | **10** |
| 2 | `PACKET_LEN` | **40**: `BOOT_POLL ... len=40`, then `BOOT_CMD53_RX: len=40` |
| 3 | 40-byte boot-frame prefix | **exact #29 match** for all 32 instrumented bytes |
| 4 | boot event | **`Received ESP boot-up event`** |
| 5 | version | **`ESP-Hosted Version: NG-1.0.6.0.1`** |
| 6 | first slave interrupt after interface-init TX | `status=0x00800000 dispatch=1 after_empty=0` |
| 7 | response length | **32 bytes** |
| 8 | complete 32-byte response | the same corrupt host-side ASCII payload as §12 |
| 9 | command `0x1` | **timed out** |
| 10 | `wlan0` | **did not appear** |

The complete response was byte-for-byte identical to the first DMA capture:

```
6d 63 30 3a 30 30 30 31 3a 31 2f 69 65 65 65 38
30 32 31 31 2f 70 68 79 30 00 41 43 54 49 4f 4e
```

That is again `mc0:0001:1/ieee80211/phy0\0ACTION`. The unchanged run therefore
reproduced the precise requested boundary: correct boot frame and boot event,
then a corrupt 32-byte command response, command `0x1` timeout and no `wlan0`.
This result licensed the 0010-only source A/B; no second unchanged repeat was
needed.

## 14. DMA with only 0010 omitted

### 14.1 Clean-build and artifact gates

`build-m1.sh` now accepts the opt-in staging control
`EASYSTICK_ESPHOSTED_DISABLE_0010=1`. It removes only
`0010-easystick-sdio-boot-packet-len-poll.patch` from the disposable
BR2_EXTERNAL copy after staging. The checked-in patch remains unchanged, so the
default build remains unchanged. The experimental build also set
`EASYSTICK_SDIO_FORCE_PIO=0` and used a brand-new empty output volume.

Clean build log:

`C:\Users\developer\tmp\easystick-p4-no0010-build-clean-20260812-r2.log`
5,715,280 bytes,
`a3b3ee8054f452c7ce2d06ca0b356c19a664cf0a58a7e38ce1382c6c54ae46f2`

The gates passed:

- Linux was freshly extracted, patched and built. The log has no `FAILED`,
  `Reversed`, fuzz or malformed-patch result.
- esp-hosted applied 0001, 0002, 0003, 0005, 0006, 0007, 0008, 0009, 0011 and
  0012. It did not apply 0010.
- The extracted module source retains 0005's exact
  `rx_byte_count = 0; msleep(100); esp_process_new_packet_intr(...)` sequence.
  It retains 0011's `after_empty` instrumentation and 0012's byte-wise CMD52
  changes, while containing no `BOOT_POLL` or 300 ms bounded polling loop.
- `easystick,force-pio` is absent from both the clean extracted kernel source
  and the clean-built `Image`.
- The clean-built module is 136,092 bytes,
  `ec95d1397c1e9a3b8608f12fffffe4a60a294c08626ca75b7f9f7e71f9caca91`,
  with `vermagic=6.18.35 riscv`.
- The clean DTB is byte-identical to the DMA baseline.

The clean `--profile m2` rootfs was not flashed: a full extraction comparison
showed unrelated profile-content differences from the actually flashed DMA
rootfs. To keep the requested variables exact, the final candidate was instead
composed from the six files of the §12 DMA candidate, replacing only
`/lib/modules/6.18.35/updates/esp32_sdio.ko` in an exact extraction of that
rootfs and recreating SquashFS with the Buildroot command. A full comparison
found 289 inodes on both sides and exactly one content/metadata difference: the
module changed from 136,204 bytes (`4ca296b8...`) to the clean-built 136,092-byte
module above. There were no added, removed or otherwise changed paths.

Final P4-only candidate:

| Artifact | SHA-256 | Relation to §12 DMA candidate |
|---|---|---|
| `bootloader.bin` | `39a09d946c6425daa7827f581d8f03ea5b3d335156d37dc3c53c0ae1c584b912` | identical |
| `partition-table.bin` | `580a0ca942e6746a768724ae7bbb486dee47a228ca46b477ee598ac45738e87f` | identical |
| `boot-shim.bin` | `9d1a25bd9b65df26681fdc782bb230aecb48895ada2de77cef2cdbc48855c353` | identical |
| `Image` | `9455e686ad4a385fd960049b0804facdd7a73936ec666b4f3eae26edfe92c634` | identical |
| `rootfs.squashfs` | `ee5dbec61385e8a63334b9755f257b142481903e6075cbefd9b03f37b8d3f016` | only `esp32_sdio.ko` differs |
| `easystick-stamp-p4.dtb` | `0fb1f66abdc4dfd19d14c7543cb6acddeb99ee72f430f42cfce9218e7c19528d` | identical |

The P4 was flashed with esptool 4.8.1. Every region passed write-side hash
verification, followed by independent digest verification for the partition
table, shim, Image, rootfs and DTB. The C6 was not written. Flash log:

`C:\Users\developer\tmp\easystick-p4-no0010-flash-20260812.log`,
`d5d78013ce2d8d615d4767f4ff841fc433533c7618bd173bc4c12aaa305f853f`

### 14.2 Strict-cold result

The detector saw COM10 detach at `2026-08-12T06:04:51.3762579Z`, return at
`2026-08-12T06:04:59.3605280Z`, and remain absent for **7.984 s**. The passive
collector then opened only after return.

`C:\Users\developer\tmp\easystick-p4-coldboot-dma-no0010-20260812.log`
16,896 bytes,
`d211beac941183b66853be5f707e695331a0308cf7fe3a505480ab7ea7a394ff`

| Recorded quantity | Value |
|---|---|
| `port_opens` | 20 |
| `first_byte_seconds` | 2.005 |
| `control_lines_driven` | **False**; `reset_requested: false` |
| Build stamp | same baseline `#1 Wed Aug 12 02:23:04 UTC 2026` Image |
| Transfer mode | **`Using internal DMA controller`** |

The requested checkpoints, again in order:

| # | Checkpoint | 0010-omitted result |
|---:|---|---|
| 1 | `Tx Pos` | **10** |
| 2 | `PACKET_LEN` | **40**: 0010's `BOOT_POLL` marker is intentionally absent; the immediate manual receive reports `BOOT_CMD53_RX: len=40` |
| 3 | 40-byte boot-frame prefix | **exact #29 match** for all 32 instrumented bytes |
| 4 | boot event | **`Received ESP boot-up event`** |
| 5 | version | **`ESP-Hosted Version: NG-1.0.6.0.1`** |
| 6 | first slave interrupt after interface-init TX | `status=0x00800000 dispatch=1 after_empty=0` |
| 7 | response length | **32 bytes** |
| 8 | complete 32-byte response | **valid and byte-identical to #29** |
| 9 | command `0x1` | **completed**: no timeout; the next 512-byte command TX followed |
| 10 | `wlan0` | **did not appear**: the 30 s `ifconfig wlan0` acceptance marker is absent, ending with `SIOCSIFFLAGS: Cannot assign requested address` |

The complete, now-correct command `0x1` response was:

```
00 00 02 00 14 00 0c 00 00 00 00 00 01 02 00 00
00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00
```

The improvement did not extend to end-to-end network initialization. The next
interrupt reported length 38, but its first 32 logged bytes were a stale copy of
the command `0x1` response above rather than #29's valid command `0x3` frame.
The following length-36 receive contained host-side ASCII
`et/wlan0/queues/tx-0`, was rejected, and command `0xF` timed out. The
`M3-lab: wlan0 appeared` marker is emitted only when `ifconfig wlan0` succeeds;
it never appeared in this run.

### 14.3 Classification

Omitting only 0010 moves the reproducible corruption boundary one command
later. It restores the first 32-byte interface-init response and lets command
`0x1` complete, but it does not eliminate the later stale/corrupt reads or
restore `wlan0` in this run. This is positive evidence that 0010's bounded
probe-time poll changes the state or ordering seen by internal-DMA receives. It
does **not** by itself prove a cache-coherency mechanism, and it is not an
end-to-end fix.

0005 was not removed: its 100 ms delay and manual
`esp_process_new_packet_intr()` path remain in the built module. DMA, 0011,
0012, the C6 firmware, DTB, reset protocol and all non-module P4 artifacts also
remained fixed, as requested.

## 15. Unchanged no-0010 repeat: new boundary is reproducible

The §14 candidate was cold-booted once more with no rebuild, reflash, source
change, C6 change, DTB change or patch change. An external 200 ms detector saw
COM10 detach at `2026-08-12T06:21:11.3638588Z`, return at
`2026-08-12T06:21:19.1368964Z`, and remain absent for **7.773 s**. The passive
90 s collector opened only after return.

`C:\Users\developer\tmp\easystick-p4-coldboot-dma-no0010-repeat2-20260812.log`
16,896 bytes,
`d211beac941183b66853be5f707e695331a0308cf7fe3a505480ab7ea7a394ff`

| Recorded quantity | Value |
|---|---|
| `port_opens` | 20 |
| `first_byte_seconds` | 2.013 |
| `control_lines_driven` | **False**; `reset_requested: false` |
| Transfer mode | **`Using internal DMA controller`** |

The raw log SHA-256 is identical to the first no-0010 A/B capture in §14.2,
which is also 16,896 bytes. The full 90 s serial output therefore reproduced
byte-for-byte, including this receive sequence:

1. The first post-boot command response reports length 32 and contains the
   valid historical command `0x1` payload:

   ```
   00 00 02 00 14 00 0c 00 00 00 00 00 01 02 00 00
   00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00
   ```

2. Command `0x1` completes: there is no `Command[0x1] timed out`, and the next
   512-byte command TX follows.
3. The next response reports length 38. Its first 32 instrumented bytes exactly
   reproduce the preceding command `0x1` payload instead of the valid #29
   command `0x3` response.
4. The next response reports length 36 and again contains the host-like ASCII
   `et/wlan0/queues/tx-0`; it is dropped as `len=8 offset=6`.
5. Command `0xF` times out.
6. `M3-lab: wlan0 appeared` is absent and the run ends with
   `ifconfig: SIOCSIFFLAGS: Cannot assign requested address`.

This is an exact reproduction rather than a materially different sequence, so
the 0010-removed DMA image is now the **reproducible baseline**. Stop here: no
additional patch is removed. The next experiment should instrument DMA RX
buffer lifecycle, ownership and advancement while keeping this baseline and
every other variable unchanged.

## 16. DMA RX buffer-lifecycle instrumentation

### 16.1 Observation-only patch and clean-build gates

Exactly one patch was added after the reproducible §15 baseline:

`0013-easystick-sdio-dma-rx-lifecycle-diagnostics.patch`, 8,351 bytes,
`3128ef9f28b15a55ff03b8fb233c26f3ddb0b8967b7fbc456f66cf60d6686370`

It adds a fixed eight-record static ring. Each existing receive records the
slave length, aligned CMD53 request length, CMD53 address, CPU virtual buffer
address, MMC host/function identity, receive producer/consumer positions,
CMD53 return value and elapsed time. FNV-1a fingerprints plus the first and
last 16 bytes are captured immediately before the existing read, immediately
after it returns, and immediately before ESP-Hosted parsing. The ring is
printed only after the existing command `0xF` timeout.

The patch adds no allocation, packet-data write, SDIO transaction, retry,
delay, poll, cache operation, lock/claim change, return/branch change or
receive call-order change. At the ESP function-driver boundary,
`sdio_memcpy_fromio()` has already discarded the MMC request/scatterlist when
it returns. Actual DMA bus address, DW-MMC IDMAC descriptor/index and
per-transfer engine are therefore recorded honestly as `unavailable`, rather
than inferred through a racy private-host cast. The runtime controller mode is
still checked independently from the boot log.

The build used `EASYSTICK_SDIO_FORCE_PIO=0` and
`EASYSTICK_ESPHOSTED_DISABLE_0010=1` in a new empty output volume. Clean log:

`C:\Users\developer\tmp\easystick-p4-rxtrace-build-clean-20260812.log`,
5,715,769 bytes,
`ea9b6ad16b4ffaef046cb74c288f2920ff6ad067abf5121d6401b8838881e084`

The gates passed:

- The staged ESP patch manifest is the §15 set plus 0013 only. The applied
  series is 0001, 0002, 0003, 0005, 0006, 0007, 0008, 0009, 0011, 0012 and
  0013; 0010 is absent. There is no failed/reversed/fuzzed/malformed hunk.
- Applied source still contains 0005's `msleep(100)` and manual
  `esp_process_new_packet_intr()` path, 0011's `after_empty` marker and 0012's
  CMD52 changes, with no `BOOT_POLL`/300 ms loop.
- A recursive comparison against the clean §15 package tree found functional
  source changes only in `esp_cmd.c` and `sdio/esp_sdio.c`, exactly matching
  the 0013 hunks. Buildroot and Linux `.config` files are SHA-identical.
- `easystick,force-pio` has zero hits in both the clean extracted kernel source
  and generated Image. The DTB is byte-identical to the fixed baseline.
- The diagnostic module is 138,468 bytes, mode 0644, uid/gid 0:0,
  `8700443bc5f29172ee045cb05ff287fe13227ecd8b9334ed27b1e28053508da5`.

As in §14, the final candidate was composed from the exact six fixed-baseline
artifacts, replacing only the module in an extraction of the fixed rootfs. A
complete inode/metadata comparison found no added, removed, type, mode,
owner/group or symlink differences. The only file-content difference is
`/lib/modules/6.18.35/updates/esp32_sdio.ko`, from the §15 hash
`ec95d139...` to the diagnostic hash above.

| Artifact | Bytes | SHA-256 | Relation to §15 baseline |
|---|---:|---|---|
| `bootloader.bin` | 22,976 | `39a09d946c6425daa7827f581d8f03ea5b3d335156d37dc3c53c0ae1c584b912` | identical |
| `partition-table.bin` | 3,072 | `580a0ca942e6746a768724ae7bbb486dee47a228ca46b477ee598ac45738e87f` | identical |
| `boot-shim.bin` | 212,112 | `9d1a25bd9b65df26681fdc782bb230aecb48895ada2de77cef2cdbc48855c353` | identical |
| `Image` | 6,568,764 | `9455e686ad4a385fd960049b0804facdd7a73936ec666b4f3eae26edfe92c634` | identical |
| `rootfs.squashfs` | 1,822,720 | `88bd12d54e9b3194f71809f6af2913abafe72a20e20b66621cafa67d149b5885` | module only |
| `easystick-stamp-p4.dtb` | 3,084 | `0fb1f66abdc4dfd19d14c7543cb6acddeb99ee72f430f42cfce9218e7c19528d` | identical |

The P4-only esptool 4.8.1 write and independent region verification passed.
The C6 was not written. Flash log:

`C:\Users\developer\tmp\easystick-p4-rxtrace-flash-20260812.log`,
14,173 bytes,
`62ac6836ef3d2e4e9b07279527f5f244d8b8d6760f60997681ac1f92fffcdf81`

### 16.2 Strict-cold capture

The 200 ms detector saw COM10 detach at `2026-08-12T07:38:41.0324448Z`,
return at `2026-08-12T07:38:46.1656148Z`, and remain absent for **5.133 s**.
The passive collector started 0.037 s after the detected return and ran for
90 s without requesting reset or driving DTR/RTS.

`C:\Users\developer\tmp\easystick-p4-coldboot-dma-rxtrace-20260812.log`,
21,796 bytes,
`a355caac6dfdd9d8af23563ff67b56021d8d631f4b53e95f8b62e814afe9ed8c`

| Recorded quantity | Value |
|---|---|
| `port_opens` | 20 |
| `first_byte_seconds` | 2.018 |
| `control_lines_driven` / `reset_requested` | **False / false** |
| Build stamp | fixed `#1 Wed Aug 12 02:23:04 UTC 2026` Image |
| Runtime transfer mode | **`Using internal DMA controller`** |

The fixed receive sequence reproduced: `Tx Pos = 10`, a successful 40-byte
historical boot frame, boot event, version `NG-1.0.6.0.1`, a normal 32-byte
command `0x1` response and completion, then lengths 38 and 36, command `0xF`
timeout, and no `wlan0`.

### 16.3 Trace result and earliest divergence

| Seq | Slave / CMD53 bytes | Buffer VA | rx before / producer / after | Elapsed | Fingerprints: pre / post / parse |
|---:|---:|---|---:|---:|---|
| 1 | 40 / 40 | `0x48c74940` | 0 / 40 / 40 | 0.530 ms | `1e2d50e9` / `4d4d7650` / `4d4d7650` |
| 2 | 32 / 32 | `0x48c75000` | 40 / 72 / 72 | 9.198 ms | `f477aed3` / `cb1dd5b8` / `cb1dd5b8` |
| 3 | 38 / 40 | `0x48c75000` | 72 / 110 / 110 | 109.250 ms | `d32a6555` / `d32a6555` / `d32a6555` |
| 4 | 36 / 36 | `0x48c75240` | 110 / 146 / 146 | 108.751 ms | `fa30a16e` / `fa30a16e` / `fa30a16e` |

All four existing CMD53 reads returned 0. Sequence 2 visibly changed the
destination during the read and produced the correct complete command `0x1`
response:

```
00 00 02 00 14 00 0c 00 00 00 00 00 01 02 00 00
00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00
```

Sequence 3 reused sequence 2's CPU virtual address. Before its CMD53 began,
the first 32 bytes were already the preceding command `0x1` payload. After the
40-byte CMD53 request returned success, all 38 logical bytes and their
fingerprint were unchanged; they were still unchanged immediately before
parsing. Its last 16 bytes were:

```
00 00 00 00 00 00 00 00 00 00 55 54 55 55 55 55
```

Sequence 4 used a different CPU virtual address, but before CMD53 it already
contained the host-like bytes `et/wlan0/queues/tx-0\0ACT`. Again, the entire
36-byte fingerprint and both edges were identical after CMD53 success and
before parsing:

```
00 00 00 00 08 00 06 00 00 00 00 00 65 74 2f 77
6c 61 6e 30 2f 71 75 65 75 65 73 2f 74 78 2d 30
00 41 43 54
```

The earliest observed bad boundary is therefore **already at DMA completion**,
not between DMA completion and ESP-Hosted parsing. For both failing receives,
the CPU-visible destination was stale before the read and did not change at
all across a successful CMD53 call. The receive counter/producer progression
did advance normally. Reuse of the sequence-2 virtual address by sequence 3 is
not, by itself, proof of IDMAC descriptor reuse; allocator reuse is possible,
and descriptor/bus information is unavailable at this driver boundary.

Classification: investigate DMA descriptor/destination advancement, DMA
destination ownership, completion semantics and noncoherent cache visibility.
There is no evidence here for a software overwrite between CMD53 completion
and parsing. Stop at this classification; no workaround or further patch was
implemented.

## 17. DW-MMC / IDMAC RX diagnostics

### 17.1 Observation-only host patch and clean-build gates

The fixed baseline remains §16: internal DMA, ESP-Hosted 0010 omitted, 0005's
100 ms delayed manual receive retained, and ESP-Hosted 0011/0012/0013 retained.
Exactly one new kernel patch was added:

`0011-easystick-dw-mmc-idmac-rx-diagnostics.patch`, 18,609 bytes,
`b3232a90cd2871370095ce15b039154264bf61bd0857d20d188831a5c46dab65`

The patch adds an eight-record static ring to DW-MMC. For CMD53 reads it records
the encoded command/address/count, CPU destination VA, mapped DMA address and
length, IDMAC ring DMA base, programmed descriptor buffer/length and relative
slot range, descriptor OWN as seen before the existing OWN poll, after that
poll, at submission, at the DMA IRQ and at request completion, and separate
controller DATA_OVER, IDMAC interrupt/completion, error, PIO-byte and final
request-result fields. Submission and completion snapshots preserve raw
RINTSTS, MINTSTS, IDSTS, BMOD, DSCADDR and BUFADDR values already available at
the host layer. Records are retrieved through a read-only
`cmd53_rx_trace` sysfs attribute after the experiment; there is no printk in
the submit/IRQ/completion path.

The patch does not read or write an RX payload byte and adds no descriptor or
controller write, cache maintenance, retry, reset, allocation in the transfer
path, delay or poll. The pre-existing 100 ms OWN poll and its existing recovery
path are unchanged; the patch only retains its return value and observed OWN
words. `checkpatch.pl --strict` reports zero errors, warnings and checks, and a
warning-enabled RISC-V cross-compile of the patch-applied `dw_mmc.o` passed.

The full M2 build used a new empty output volume with
`EASYSTICK_SDIO_FORCE_PIO=0` and
`EASYSTICK_ESPHOSTED_DISABLE_0010=1`:

`C:\Users\developer\tmp\easystick-p4-idmactrace-build-clean-20260812.log`,
5,716,171 bytes,
`87fe2490833e4a2de64addd85c3706607674b7ef07c5f5942a92b78bef91fb50`

The clean-build gates passed:

- Linux `Extracting` and `Patching` both ran. The local kernel series is the
  §16 set plus this 0011 only; force-PIO 0008 is absent. The new host patch
  applies once to headers and once to the kernel build tree, with no failed,
  reversed, fuzzed or malformed hunk.
- The ESP-Hosted applied series remains 0001, 0002, 0003, 0005, 0006, 0007,
  0008, 0009, 0011, 0012 and 0013. ESP-Hosted 0010 is absent. Applied
  ESP-Hosted C/header/build sources and `esp32_sdio.ko` are byte-identical to
  §16; the module hash remains `8700443b...`.
- Kernel `.config` and DTB are byte-identical to §16. Both extracted
  `dw_mmc.c`/`.h` files match the reviewed patch result byte-for-byte.
  `easystick,force-pio` has zero hits in source and Image, while the Image
  contains both `cmd53_rx_trace` and `CMD53HOST seq=`.
- The clean rebuild's rootfs was not substituted: the flash candidate reuses
  the exact currently-flashed §16 boot chain, rootfs, DTB and ESP-Hosted
  module, and replaces only Image.

| Artifact | Bytes | SHA-256 | Relation to §16 |
|---|---:|---|---|
| `bootloader.bin` | 22,976 | `39a09d946c6425daa7827f581d8f03ea5b3d335156d37dc3c53c0ae1c584b912` | identical |
| `partition-table.bin` | 3,072 | `580a0ca942e6746a768724ae7bbb486dee47a228ca46b477ee598ac45738e87f` | identical |
| `boot-shim.bin` | 212,112 | `9d1a25bd9b65df26681fdc782bb230aecb48895ada2de77cef2cdbc48855c353` | identical |
| `Image` | 6,576,956 | `67a175307c93fdb88fced80fef8ba56404a6409c00d9d0615d4c32f2edf6abbc` | **DW-MMC diagnostics only** |
| `rootfs.squashfs` | 1,822,720 | `88bd12d54e9b3194f71809f6af2913abafe72a20e20b66621cafa67d149b5885` | identical |
| `easystick-stamp-p4.dtb` | 3,084 | `0fb1f66abdc4dfd19d14c7543cb6acddeb99ee72f430f42cfce9218e7c19528d` | identical |

The P4-only esptool 4.8.1 write and independent verification passed. The C6 was
not written. Flash log:

`C:\Users\developer\tmp\easystick-p4-idmactrace-flash-20260812.log`,
13,562 bytes,
`2100f99856fdebcec8982e652314859852ab318bd2e1204e9bf8d81baf922bfa`

### 17.2 Strict-cold capture

One preliminary detector attempt observed 4.729 s of COM10 absence and was
rejected before collection because it missed the explicit 5.000 s gate. No
image or source changed. The accepted 200 ms detector then saw COM10 detach at
`2026-08-12T09:07:29.4146905Z`, return at
`2026-08-12T09:07:41.2594650Z`, and remain absent for **11.845 s**. The passive
collector started on return and ran for 90 s without requesting reset or
driving DTR/RTS.

`C:\Users\developer\tmp\easystick-p4-coldboot-dma-idmactrace-20260812.log`,
18,240 bytes,
`486fd945f6a6c18c12d7d95ae349ae7bd3eabe3333ec021b71f214aa77b7176c`

| Recorded quantity | Value |
|---|---|
| `port_opens` | 20 |
| `first_byte_seconds` | 2.014 |
| `control_lines_driven` / `reset_requested` | **False / false** |
| Build stamp | `#1 Wed Aug 12 08:48:43 UTC 2026` |
| Runtime transfer mode | **`Using internal DMA controller`** |

The boot frame remained correct: `Tx Pos = 10`, logical/CMD53 length 40 and the
instrumented 32-byte prefix exactly match historical #29. The C6 reports
`POWERON_RESET`; the boot event and version `NG-1.0.6.0.1` are present.

Unlike the reproducible §15/§16 failure, this run's subsequent receive sequence
did **not** reproduce stale or host-ASCII payloads:

1. The 32-byte command `0x1` response is normal and command `0x1` completes.
2. The next logical-38/CMD53-40 response begins with the correct command `0x3`
   header/payload, rather than duplicating command `0x1`.
3. The next 36-byte response begins with the correct command `0xF`
   header/payload, rather than host-like `et/wlan0/queues/tx-0` ASCII.
4. Command `0xF` completes far enough for the driver to decode and print
   capabilities. No command timeout is logged.
5. `wlan0` still does not appear during the 90 s window; output stops after
   capability decoding and `random: crng init done`.

Thus the failing 38-/36-byte comparison required for the requested host-layer
classification is absent in this accepted run. No further boot, source change
or workaround had been performed at that point. The saved DW-MMC ring remained
available through the read-only platform-device sysfs attribute, but that fixed
rootfs offered a shell only on `ttyS0`; COM10 was `ttyGS1` console output and
could not read the attribute. Section 17.3 records the subsequent, isolated
rootfs-only shell-access change and the passive snapshot obtained after a
second accepted strict-cold boot.

### 17.3 Isolated `ttyGS1` shell access

The module USB-C uses the P4's fixed USB Serial/JTAG controller and its single
EP1 serial channel. It is not Linux's generic USB gadget/composite framework,
so another CDC ACM function cannot be added to that connector by instantiating
another `ttyGS` device. The carrier USB-A is wired to the independent USB1 OTG
GPIO26/GPIO27 pair, but the current native-Linux image has `CONFIG_USB_SUPPORT`
disabled; a multi-CDC USB1 gadget is a separate kernel/UDC/DTB experiment and
was deliberately excluded from this fixed SDIO comparison.

To retrieve the already-buffered host ring through COM10, one rootfs source
line was added to the base overlay's `/etc/inittab`:

```
ttyGS1::askfirst:-/bin/sh
```

This exposes an unauthenticated root shell to a physically attached module
USB-C cable and is suitable only for the lab diagnostic image. It does not
change Image, DTB, C6 firmware, ESP-Hosted, SDIO timing, bus width, reset
behavior, DMA mode or receive logic.

The exact section 17.1 rootfs was expanded twice. The line above was copied
into one expansion while preserving the original `inittab` mode, owner, group
and mtime, then it was repacked with the same SquashFS block size, LZ4 `-Xhc`
compression and original filesystem creation epoch. A second expansion of the
result was compared with the untouched expansion. There are no added, removed,
type, mode, owner/group, timestamp or symlink changes. The sole metadata
difference is `/etc/inittab` size, 98 to 124 bytes, and the sole content
difference is that file. Composition log:

`C:\Users\developer\tmp\easystick-p4-idmactrace-shell-rootfs-compose-20260812.log`,
5,714 bytes,
`378a29e965c0dd4d9a906280d1e912fa088ac732c44146c3a9f9dc2a84421717`

The resulting candidate preserves five section 17.1 artifacts byte-for-byte:

| Artifact | Bytes | SHA-256 | Relation to section 17.1 |
|---|---:|---|---|
| `bootloader.bin` | 22,976 | `39a09d946c6425daa7827f581d8f03ea5b3d335156d37dc3c53c0ae1c584b912` | identical |
| `partition-table.bin` | 3,072 | `580a0ca942e6746a768724ae7bbb486dee47a228ca46b477ee598ac45738e87f` | identical |
| `boot-shim.bin` | 212,112 | `9d1a25bd9b65df26681fdc782bb230aecb48895ada2de77cef2cdbc48855c353` | identical |
| `Image` | 6,576,956 | `67a175307c93fdb88fced80fef8ba56404a6409c00d9d0615d4c32f2edf6abbc` | identical |
| `rootfs.squashfs` | 1,822,720 | `c9d5e27237005a452ec45a52d2b838efc5ba84c672b832b2758b01f7ccbd5d01` | `inittab` only |
| `easystick-stamp-p4.dtb` | 3,084 | `0fb1f66abdc4dfd19d14c7543cb6acddeb99ee72f430f42cfce9218e7c19528d` | identical |

The P4-only esptool 4.8.1 write and independent verification passed for all
non-bootloader regions; every write-side hash also passed. The C6 was not
written. Flash log:

`C:\Users\developer\tmp\easystick-p4-idmactrace-shell-flash-20260812.log`,
14,305 bytes,
`6320f2efb67730c1e8afe5455c133907daccdb45011c0fc4dc92fa3bca89d48b`

### 17.4 Second strict-cold capture and host-ring classification

The 200 ms detector saw COM10 detach at `2026-08-12T09:29:51.8483238Z`,
return at `2026-08-12T09:30:05.5694122Z`, and remain absent for **13.721 s**.
The passive collector started on return and ran for 90 s without requesting a
reset or driving DTR/RTS.

`C:\Users\developer\tmp\easystick-p4-coldboot-dma-idmactrace-shell-20260812.log`,
19,029 bytes,
`70d083923fcb18f9589d9c07a5977e674ce714d91598837ecfb39cf63473bb62`

| Recorded quantity | Value |
|---|---|
| `port_opens` | 20 |
| `first_byte_seconds` | 2.016 |
| `control_lines_driven` / `reset_requested` | **False / false** |
| Build stamp | unchanged `#1 Wed Aug 12 08:48:43 UTC 2026` Image |
| Runtime transfer mode | **`Using internal DMA controller`** |

The boot frame again has `Tx Pos = 10`, logical/CMD53 length 40 and the exact
historical #29 prefix. The boot event, `POWERON_RESET` and version
`NG-1.0.6.0.1` are present. This boot then reproduced the decisive middle
failure: the normal 32-byte command `0x1` response completed, while the next
logical-38/CMD53-40 payload repeated the preceding command `0x1` bytes. The
following 36-byte command `0xF` response was correct and capability decoding
completed without a command timeout. `wlan0` did not appear.

After the 90 s evidence window, COM10 was reopened with DTR and RTS false. Only
the waiting `askfirst` shell and the read-only
`/sys/bus/platform/devices/50083000.mmc/cmd53_rx_trace` attribute were used; no
SDIO command, reset, rescan, module operation or controller-register read was
issued. Saved snapshot:

`C:\Users\developer\tmp\easystick-p4-idmactrace-shell-sysfs-20260812.log`,
2,381 bytes,
`0c5f53c3c40cafc2a7a4774ad229a443ff7c9523f250cc071f9ff39d6ad7b0e6`

| Seq | Logical / CMD53 bytes | CPU VA / mapped DMA | Descriptor state before submit | Engine / elapsed | Completion |
|---:|---:|---|---|---|---|
| 1 | 40 / 40 | `0x48c74940` / `0x48c74940` | slot 0, OWN `00000000 -> 8000000c` | IDMAC / 0.475 ms | DTO=1, DMA=1, 40 bytes, err=0 |
| 2 | 32 / 32 | `0x48c75000` / `0x48c75000` | slot 0, OWN `0000000c -> 8000000c` | IDMAC / 0.448 ms | DTO=1, DMA=1, 32 bytes, err=0 |
| 3 | 38 / 40 | `0x48c75000` / `0x48c75000` | OWN stayed `8000000c`; 100 ms poll timed out; no descriptor built | PIO fallback / 108.714 ms | DTO=1, DMA=0, 40 PIO bytes, err=0 |
| 4 | 36 / 36 | `0x48c75240` / `0x48c75240` | slot 0, OWN `0000000c -> 8000000c` | IDMAC / 0.439 ms | DTO=1, DMA=1, 36 bytes, err=0 |

For sequence 2, DMA mapping returns one SG entry, one descriptor is programmed
with the expected destination and length, IDMAC start returns 0, and separate
DATA_OVER and IDMAC-complete interrupts are observed (`IDSTS=0x0000a102`).
At its IRQ and final software snapshot the CPU still reads descriptor OWN set.

Sequence 3 diverges **before DMA submission**. The same ring entry begins with
OWN `0x8000000c` and the driver's existing 100 ms OWN-clear poll still reads
`0x8000000c` at timeout. Descriptor preparation returns `-EINVAL` (`start=-22`),
so the record has descriptor count 0, no programmed descriptor buffer/length,
no IDMAC interrupt, and the existing driver falls back to PIO. MMC reports
40 bytes and `err=0`, but the ESP-Hosted payload repeats sequence 2. Sequence 4
then sees OWN clear and successfully submits the same relative slot through
IDMAC again.

The earliest identified bad host-controller state is therefore **descriptor
ownership not being returned/observed before the failing 40-byte receive**.
The controller is not given a new DMA destination descriptor for that transfer;
this rules out a wrong DMA destination programmed by sequence 3. The present
trace does not distinguish a controller descriptor-retirement/advancement bug
from stale CPU visibility of the noncoherent descriptor ring. That is the next
investigation boundary. No cache sync, flush, descriptor reset, retry, delay,
buffer clear or other workaround was added.

## 18. Static audit of the ESP32-P4 DMA coherency path

No image was rebuilt or flashed for this section, and no source file was
changed. The device still carries the §17.3 diagnostic image
(`Image` `67a17530...`, `rootfs.squashfs` `c9d5e272...`). This section is
source reading plus re-reading of the §17.4 snapshot already on disk.

### 18.1 The question

§17.4 left two candidate explanations for `start=-22` and could not separate
them:

1. IDMAC really did not return descriptor ownership.
2. IDMAC returned ownership, and the CPU was reading a stale descriptor
   cache line.

Branch 2 is only possible if the descriptor ring is ordinary cached RAM. The
first half of this audit determines which kind of memory the ring actually is;
the second half re-reads the trace for the signature each branch predicts.

### 18.2 The IDMAC descriptor ring is not coherent memory on this port

Upstream allocates the ring with `dmam_alloc_coherent()`
(`vendor/linux/drivers/mmc/host/dw_mmc.c:3134`). On this configuration that
call **fails**, and the failure is visible in every DMA boot log:

```
coherent DMA allocations not supported on this platform.
IDMAC supports 32-bit address mode.
IDMAC: cached descriptor ring with manual cache sync
Using internal DMA controller.
```

(`C:\Users\developer\tmp\easystick-p4-coldboot-dma-idmactrace-shell-20260812.log`.)

The chain that produces it, read end to end:

| Step | Source | Consequence here |
|---|---|---|
| `sdmmc1` has no `dma-coherent` property | `firmware/linux/dts/easystick-stamp-p4.dts` | `dev_is_dma_coherent()` false |
| `dma_direct_alloc()` non-coherent branch needs one of `ARCH_HAS_DMA_ALLOC`, `DMA_GLOBAL_POOL`, `ARCH_HAS_DMA_SET_UNCACHED`, `DMA_DIRECT_REMAP` | `vendor/linux/kernel/dma/direct.c` | none is available |
| `RISCV_ISA_ZICBOM` `depends on MMU` and is what `select`s `DMA_DIRECT_REMAP` | `vendor/linux/arch/riscv/Kconfig` | excluded by `CONFIG_MMU=n` |
| RISC-V defines no `ARCH_HAS_DMA_SET_UNCACHED` | `arch/riscv/` | no uncached aperture |
| result | `pr_warn_once("coherent DMA allocations not supported…")`, `return NULL` | `dmam_alloc_coherent()` returns NULL |
| why2025 `0007-mmc-dw_mmc-esp32p4-fixes.patch` fallback | `devm_kzalloc()` + `virt_to_phys()`, prints `IDMAC: cached descriptor ring with manual cache sync` | ring is **plain cached kernel RAM** |

So the answer to the audit's primary question is unambiguous: **the ring is
ordinary cached RAM whose visibility depends entirely on explicit
platform-specific cache operations.** The observed ring base
`0x48e2a040` in the §17.4 trace is 64-byte aligned, which matches the
`devm_kzalloc` fallback and the ESP32-P4 cache block size.

### 18.3 The platform cache operations exist and are registered

`why2025-linux` `0001-riscv-esp32p4-baseline.patch` adds
`drivers/cache/esp32p4_cache.c`, which calls two ROM thunks
(`writeback 0x4fc003f4`, `invalidate 0x4fc003e4`) across the L1D and L2 maps
and registers them through `riscv_noncoherent_register_cache_ops()` at
`arch_initcall`. The boot log confirms registration:

```
esp32p4_cache: registered ROM-thunk noncoherent ops, block=64
```

`arch_sync_dma_for_device` / `arch_sync_dma_for_cpu`
(`vendor/linux/arch/riscv/mm/dma-noncoherent.c:69` and `:98`) dispatch to those
ops when `CONFIG_RISCV_NONSTANDARD_CACHE_OPS=y`, which this build sets. Nothing
in the cache-ops layer is missing or stubbed.

### 18.4 The defect: the ring is written back but never invalidated

Every cache maintenance operation performed on the descriptor ring in this
entire build is a single line, added by why2025 `0007`, inside
`dw_mci_idmac_start_dma()`:

```c
dma_sync_single_for_device(host->dev, host->sg_dma,
                           DESC_RING_BUF_SZ, DMA_TO_DEVICE);
```

`DMA_TO_DEVICE` reaches `arch_dma_cache_wback()` only
(`dma-noncoherent.c:73-75`) — a **clean**, not an invalidate. That is correct
and necessary for the CPU→IDMAC direction: the descriptors the CPU just wrote
must reach memory before the engine reads them.

There is no operation anywhere for the opposite direction:

- Upstream `dw_mmc.c` contains exactly two `dma_sync_*` sites, lines 483 and
  826, and **both are in the EDMAC path**. The IDMAC path has no cache
  maintenance at all upstream, because upstream assumes
  `dmam_alloc_coherent()` succeeded.
- No local kernel patch (`firmware/linux/kernel-patches/`) adds one; 0011 is
  observation-only by construction.
- No why2025 patch staged by `build-m1.sh` adds one.
  (`0015-riscv-esp32p4-cache-thunk-hardening.patch` exists in that submodule
  but is **not** in this build's patch list.)

The OWN poll that produces `start=-22` reads the descriptor with
`readl_poll_timeout_atomic(&desc->des0, …)`
(`dw_mmc.c:606` for 32-bit descriptors, `:678` for 64-bit). `readl` on ordinary
RAM is a normal load; it does not invalidate a cache line. So the OWN-clear
that IDMAC writes into physical memory has **no defined path** into the CPU's
L1D/L2 view. The ring is synchronised in one direction only.

### 18.5 The payload path is handled correctly, and is not the same defect

For transfers that do enter IDMAC, the RX payload is covered:
`dma_map_sg(…, DMA_FROM_DEVICE)` performs the pre-DMA writeback, and
`dw_mci_dmac_complete_dma()` (`dw_mmc.c:473`) reaches
`dw_mci_dma_cleanup()` → `dma_unmap_sg()`, whose `arch_sync_dma_for_cpu`
does the post-DMA invalidate (`dma-noncoherent.c:105-110`). Sequences 1, 2 and
4 in the §17.4 trace all delivered correct payloads, consistent with this.
The descriptor-ring asymmetry in §18.4 has no counterpart on the payload
buffers.

### 18.6 The trace already shows completion with OWN still set

Re-reading `easystick-p4-idmactrace-shell-sysfs-20260812.log` against the
0011 field order `own = own_before / own_after_poll / own_submit /
own_dma_irq / own_finish`:

| Seq | own_before | own_after_poll | own_submit | own_dma_irq | own_finish | IDSTS at IRQ | Result |
|---:|---|---|---|---|---|---|---|
| 1 | `00000000` | `00000000` | `8000000c` | `8000000c` | `8000000c` | `0000a102` | 40 B, err=0 |
| 2 | `0000000c` | `0000000c` | `8000000c` | `8000000c` | `8000000c` | `0000a102` | 32 B, err=0 |
| 3 | `8000000c` | `8000000c` | — | — | — | `00000000` | `start=-22`, PIO |
| 4 | `0000000c` | `0000000c` | `8000000c` | `8000000c` | `8000000c` | `0000a102` | 36 B, err=0 |

The question the audit was asked to settle — *does the successful 32-byte
transfer already report DMA completion while the CPU-visible descriptor still
has OWN set?* — is answered **yes**, and not only for the 32-byte transfer:
it holds for all three successful IDMAC transfers, 3 of 3.

Sequence 2 is the clearest single case. At its IDMAC interrupt the controller
reports `IDSTS=0x0000a102` (RI/NIS), `dto=1`, `dma=1`, `dataerr=0`; at request
completion `DSCADDR=0x48e2a040` (the ring base, i.e. the engine has retired
past that descriptor), `BUFADDR=0x48c75020` (the destination advanced by the
full 32 bytes), `bytes=32`, `err=0`. The transfer is complete by every
controller-side measure. Yet `own_dma_irq` and `own_finish` both read
`0x8000000c` — OWN still set, and bit-for-bit the exact word the CPU itself
wrote at submit. It is never a partially-updated value.

The CPU first observes the clear one transfer later, as the *next* record's
`own_before` (`0x0000000c` at seq 2, 4). Sequence 3 is the case where that
late refresh does not arrive: `own_before` and `own_after_poll` are both
`0x8000000c` across the full 100 ms poll, and the driver takes `err_own_bit`
(`dw_mmc.c:641` / `:715`) → `memset` + `dw_mci_idmac_init()` → `-EINVAL`
→ PIO.

A functioning controller that has already signalled RI/NIS and advanced both
`DSCADDR` and `BUFADDR` has, by the IDMAC descriptor contract, released
ownership in memory. The word the CPU reads instead is precisely its own last
store to that line. That is the signature of branch 2, and it is present in
every successful transfer rather than only in the failure — which is why it
went unnoticed: while the stale line happens to refresh in time, the defect is
invisible.

### 18.7 Classification after the static audit

- The descriptor ring is **not** coherent; it is cached RAM (§18.2).
- The only maintenance applied to it is a writeback; no invalidate exists
  anywhere in this build (§18.4).
- Completion is reported while the CPU-visible OWN is still set, in 3 of 3
  successful transfers (§18.6).

Taken together these are consistent with the user's reframing: the 38-byte /
CMD53-40 transfer was **never submitted to IDMAC**. The corrupt payload
observed at the ESP-Hosted layer is downstream of a non-submission, not the
product of a bad DMA write.

The audit narrows the two branches but does not close them by reading alone.
Nothing here proves the controller *did* clear OWN in memory; it proves that
if the controller had cleared it, this build has no mechanism by which the CPU
would see it. One controlled A/B separates the two.

### 18.8 What this audit cannot see

- It cannot observe physical memory. Every OWN value in the trace is a CPU
  load and therefore subject to the same cache view under audit. A bus-side
  or JTAG-side read of `0x48e2a040` would be independent evidence; none was
  taken.
- It cannot rule out a controller descriptor-retirement bug. `DSCADDR` and
  `BUFADDR` advancing is strong evidence of engine progress but is read from
  controller registers, not from the descriptor.
- It does **not** explain sequence 3's stale payload. The obvious theory —
  that `dma_unmap_sg` invalidates the PIO-written lines — is not supported by
  the code as read: `dw_mci_submit_data_dma()` calls `dma_ops->stop()` on
  start failure (`dw_mmc.c:1129`) but **not** `dw_mci_dma_cleanup()`, and
  `dw_mci_dma_cleanup()` (`:435`) only unmaps when
  `host_cookie == COOKIE_MAPPED`, so the sg stays mapped through the PIO
  fallback and nothing unmaps it on the non-error completion path. An equally
  consistent alternative is that the 108.7 ms stall desynchronised the
  ESP-Hosted/C6 RX sequencing and the slave re-sent the previous payload. The
  host trace cannot distinguish these. Recorded as unresolved.
- It cannot see anything about the C6 side, SDIO bus timing, or whether the
  38-byte logical length is itself significant.

### 18.9 The single controlled A/B, specified but not built

Not built and not flashed. This subsection is the specification only.

**Baseline.** Unchanged §17.3 configuration: internal DMA,
`EASYSTICK_SDIO_FORCE_PIO=0`, `EASYSTICK_ESPHOSTED_DISABLE_0010=1`, kernel
patch 0011 retained, ESP-Hosted series unchanged.

**Variable.** Exactly one: a CPU-visibility operation on the descriptor's own
cache line, applied **inside** the existing OWN poll, before each re-read of
`des0`, in `dw_mci_prepare_desc32()` and `dw_mci_prepare_desc64()`.

Placement matters and is the one design decision here. Invalidating once
before entering the poll is not sufficient: `readl_poll_timeout_atomic()` would
then spin for the full 100 ms on a line that is stale again after the first
iteration, and branch 2 would present as branch 1 — a false negative that
would look like a clean result. The operation must be inside the loop body.

**Scope.** One 64-byte line, not the whole ring:
`host->sg_dma + (desc - desc_first) * sizeof(struct idmac_desc)`, length
`sizeof(struct idmac_desc)` (16 bytes, so one ESP32-P4 cache block). The ring
base is 64-byte aligned, so that block lies entirely inside the ring — it can
touch at most four ring descriptors and no neighbouring kernel data.

**Hazard to record in the experiment, not to fix.** An invalidate reached
immediately after `err_own_bit`'s `memset(host->sg_cpu, 0, DESC_RING_BUF_SZ)`
and `dw_mci_idmac_init()` would discard still-dirty CPU writes. In practice
this is benign — memory still holds a valid forward-linked ring and
`dw_mci_prepare_desc32()` rewrites `des0`/`des1`/`des2` before use — but it
must be stated in the patch header rather than discovered later.

**Explicitly not in this experiment:** no payload-buffer synchronisation, no
retry, no delay, no descriptor reset, no poll-timeout change, no change to
`err_own_bit` recovery, no ESP-Hosted change, no rootfs change.

**Discriminator.**

| Outcome | Reading |
|---|---|
| The 38/40-byte receive enters IDMAC (`start=0`, `count=1`, `engine=idmac`) | Branch 2. Descriptor coherency is causally implicated. **Stop**; do not proceed to payload coherency in the same step. |
| OWN remains `0x8000000c` after the visibility operation | Branch 1. Move to controller descriptor writeback / completion semantics. |

A useful secondary signal either way: with the invalidate in place,
`own_dma_irq` and `own_finish` on the *successful* transfers should read
`0x0000000c` rather than `0x8000000c`. If they still read `0x8000000c`, the
visibility operation itself is not reaching the line, and neither row of the
table above may be believed.

The long-term correct fix — a genuinely coherent ring via
`CONFIG_DMA_GLOBAL_POOL` plus a reserved uncached DT region — is out of scope
for this A/B and is not to be attempted as part of it.

### 18.10 Scope of the `ttyGS1` root shell

`ttyGS1::askfirst:-/bin/sh` is an unauthenticated root shell reachable from a
physical USB-C connection alone. Where it currently lives, measured rather
than assumed:

| Overlay | Contains the line | Applies to |
|---|---|---|
| `buildroot-external/board/easystick-stamp-p4/rootfs-overlay/etc/inittab` | **added by `c893dfa`** | base overlay: m1, m2, and first in the list for m3/m3-lab |
| `m3/rootfs-overlay/etc/inittab` | pre-existing | m3, m3-lab |
| `m3-lab/rootfs-overlay/etc/inittab` | pre-existing, with a lab-console comment | m3-lab |

Buildroot applies overlays in order and later files win, so m3 and m3-lab
replace `/etc/inittab` wholesale with their own copies. The `c893dfa` edit
therefore takes effect on **m1 and m2 only** — which is what this SDIO
investigation needs — and does not add exposure to m3 or m3-lab, which already
carried the same line by their own earlier decision.

Handling, per the constraint recorded with this investigation: the base-overlay
line is for the isolated experiment environment only. It must be removed from
`buildroot-external/board/easystick-stamp-p4/rootfs-overlay/etc/inittab` before
Wi-Fi/SSH acceptance testing or integration into `main`; the pre-existing m3 /
m3-lab occurrences are a separate decision and are not in scope here. Nothing
was changed in this section.

## 19. Descriptor-coherency A/B: 0012 conditional descriptor invalidate

Single-variable experiment against the §17.3/§17.4 no-0010 IDMAC-trace
baseline. One switch was newly enabled, `EASYSTICK_IDMAC_DESC_INVALIDATE`;
everything else — ESP-Hosted, C6 firmware, DTB, reset behaviour, SDIO timing,
payload-buffer cache handling, PIO fallback, descriptor reset, the no-0010
baseline and the 0011 diagnostics — is unchanged and was verified unchanged by
hash before flashing.

### 19.1 The change

`0012-easystick-dw-mmc-idmac-desc-invalidate.patch`, 3606 bytes, SHA-256
`716793e1663591ede5c737482bf0a4b988e925d06566209527f717b5e80b3681`, three
hunks, `drivers/mmc/host/dw_mmc.c` only. checkpatch: 0 errors, 1 warning
(missing commit description, as sibling `0010` also reports).

It replaces the two `readl_poll_timeout_atomic(&desc->des0, ...)` OWN polls
with `read_poll_timeout_atomic()` over an accessor:

```c
u32 val = readl(&desc->des0);
if (val & IDMAC_DES0_OWN) {          /* IDMAC_OWN_CLR64(val) for the 32-bit ring */
	dw_mci_idmac_desc_invalidate(host, desc, sizeof(*desc));
	val = readl(&desc->des0);
}
return val;
```

`dw_mci_idmac_desc_invalidate()` computes the descriptor's offset within the
ring, aligns down/up to `L1_CACHE_BYTES` (64 on this part) and calls
`dma_sync_single_for_cpu(..., DMA_FROM_DEVICE)` on that one line. The 10 µs
interval and 100 ms timeout are carried over unchanged, so the polling cadence
is identical to the baseline.

Two properties are deliberate and were agreed before the run:

* **Conditional, not unconditional.** An unconditional invalidate before the
  first read would discard the still-dirty ring links written by
  `dw_mci_idmac_init()` before their writeback, which could break IDMAC and
  make the experiment uninterpretable. Read-first, invalidate-only-if-OWN-set
  cannot lose a dirty line the CPU still owns exclusively.
* **Poll site only.** No invalidate at the IRQ or completion diagnostic reads,
  and no whole-ring invalidate. `own_dma_irq` and `own_finish` therefore
  remain `0x8000000c` **by construction** and are not validity controls for
  this A/B. The working control is `own_before` (never invalidated) against
  `own_after_poll` (invalidated), on the same descriptor, in the same poll.

`dw_mmc.h` is untouched, so the 0011 trace layout and sysfs format are
byte-comparable with §17.4.

### 19.2 Build and flash gates

`0012` applied exactly twice in the clean build (linux-headers custom, linux
custom), each touching only `dw_mmc.c`. The full-log scan for patch failure,
reversal, rejection, fuzz and malformed-patch markers was empty.
`0008-easystick-dw-mmc-force-pio` is absent from the applied series;
`easystick,force-pio` has 0 hits in the source and 0 in the `Image`.

Byte-identity against the preserved §17.1 reference set, extracted to
`C:\Users\developer\tmp\easystick-p4-ref-17-1-20260812\` before its build
volume was retired:

| Artifact | Result |
|---|---|
| `dw_mmc.h` | identical |
| `.config` (`5169cacc…`) | identical |
| `easystick-stamp-p4.dtb` (`0fb1f66a…`) | identical |
| `esp32_sdio.ko` (`8700443b…`) | identical |
| ESP-Hosted applied sources | identical by content, 42 files, `diff -r` clean |
| `dw_mmc.c` | `01644597…613038`, matches the reviewed post-0012 work file |
| `Image` | **differs, as required** |

Source delta 64 added / 8 removed; the 8 removed lines are exactly the two old
`readl_poll_timeout_atomic` polls. `cmd53_rx_trace` and `CMD53HOST seq=` each
appear once in the `Image`, matching §17.1.

Flash: the `chip_id` gate passed on the first attempt (no strap improvised, no
manual button — this module has none), MAC `e8:f6:0a:e2:5e:73` confirming
COM10 addressed the P4. 6/6 write-side `Hash of data verified`; 5/5
`verify_flash` digests matched (`bootloader.bin` excluded by design, esptool
rewrites its header). The C6 was not written; the DTB was rewritten with the
byte-identical file. On-board, only `Image` changed, to
`0cacf3805c8732cdab82324160bf3b74a8a8bc1fd2b1d7a0cc9df0da67319b60`.
Log `easystick-p4-descinv-flash-20260812.log`, 14164 bytes,
`16c24ea750b62aacb6078ab3b7e3d9b5fa33644e863503c322505c60cd90a838`.

### 19.3 Strict-cold evidence

The watcher was armed **before** the disconnect, so the absence interval is
measured rather than assumed. While waiting it used only
`SerialPort::GetPortNames()` and never opened COM10.

| Quantity | Value |
|---|---|
| Detach (UTC) | `2026-08-12T12:17:36.4596203Z` |
| Return (UTC) | `2026-08-12T12:17:52.3996398Z` |
| Absence | **15.94 s** (gate ≥ 5.000 s) |
| Accepted | true |
| `control_lines_driven` / `reset_requested` | **false / false** |
| Port opens during capture | 1 |
| First byte after open | 0.068 s |
| Capture duration | 90.014 s |
| Raw log | 19080 bytes, `4d9329570723c7f9e2411305b0fc226a1769d9232274e5687215f47f4b923d5d` |

Boot provenance: the P4 prints `rst:0x17 (CHIP_USB_UART_RESET)` because the
single passive open landed 68 ms after enumeration, and **the C6 reports
`POWERON_RESET`** — which is §17.2's actual acceptance criterion, so this run
is methodologically equivalent to the accepted §17.2/§17.4 runs. Kernel stamp
`#1 Wed Aug 12 11:49:53 UTC 2026` matches this build.

The read-only shell session ran afterwards on the same powered-up board. It
opened COM10 with DTR/RTS false and listened passively for 5 s first: **no boot
banner appeared**, the prompt was already at `~ #`, and `dmesg` still holds the
complete boot — so the port open did not reset the board, and the trace read
below belongs to the strict-cold boot rather than to a later one. Shell log
6919 bytes, `d36d6c56f4982ef2f93c71f730d94fc03b9d131ed253ae809a7e14f2db22b70a`.

### 19.4 Shell-side evidence block

```
Linux (none) 6.18.35 #1 Wed Aug 12 11:49:53 UTC 2026 riscv32 GNU/Linux
earlycon=esp32p4usbjtag,mmio32,0x500d2000 console=ttyGS1,115200n8 keep_bootcon
  loglevel=8 root=/dev/mtdblock0 rootfstype=squashfs ro init=/sbin/init idle=poll

dw_mmc 50083000.mmc: IDMAC supports 32-bit address mode.
coherent DMA allocations not supported on this platform.
dw_mmc 50083000.mmc: IDMAC: cached descriptor ring with manual cache sync
dw_mmc 50083000.mmc: Using internal DMA controller.
dw_mmc 50083000.mmc: Version ID is 270a
mmc0: new SDIO card at address 0001
esp32_sdio: read_packet: BOOT_CMD53_RX: len=40 ret=0
esp32_sdio: esp_validate_chipset: Chipset=ESP32-C6 ID=0d detected over SDIO
esp32_sdio: esp_probe: ESP SDIO probe completed
esp32_sdio: read_packet: BOOT_CMD53_RX: len=32 ret=0
esp32_sdio: read_packet: BOOT_CMD53_RX: len=38 ret=0
esp32_sdio: BOOT_CMD53_DATA: 00 00 00 00 10 bd a3 9e 22 f4 00 00 00 00 00 00
esp32_sdio: read_packet: BOOT_CMD53_RX: len=36 ret=0
esp32_sdio: print_capabilities:  * WLAN on SDIO / - HCI over SDIO

2: wlan0: <BROADCAST,MULTICAST,UP,LOWER_UP> mtu 1500 qdisc pfifo_fast qlen 1000
    link/ether 10:bd:a3:9e:22:f4 brd ff:ff:ff:ff:ff:ff
esp32_sdio 65536 - - Live 0x48ef0000 (O)
```

Resolved sysfs path: `/sys/bus/platform/devices/50083000.mmc/cmd53_rx_trace`.
`find /sys -type f -name cmd53_rx_trace -print` returned nothing — a busybox
`find` limitation on sysfs, not a missing attribute; `cat` on the resolved path
succeeded. `modinfo esp32_sdio` returned nothing (no applet / no `.ko` in this
minimal rootfs); per the experiment's own terms that is not a failure.

**`wlan0` is present and UP**, MAC `10:bd:a3:9e:22:f4` — the same six bytes
carried by the 38-byte payload that §17.4's baseline failed to receive.

### 19.5 Per-transfer results

All four records, strict-cold boot, `cmd53_rx_trace`:

| Seq | Bytes | own_before | own_after_poll | own_timeout | start | Engine | IDSTS @ IRQ | dto/dma/err | done | bytes_xfered |
|---:|---:|---|---|---:|---:|---|---|---|---|---:|
| 1 | 40 | `00000000` | `00000000` | 0 | 0 | `idmac` | `0000a102` | 1/1/0 | `dsc=0x48e2a040 buf=0x48c74968 status=00000008` | 40 |
| 2 | 32 | `0000000c` | `0000000c` | 0 | 0 | `idmac` | `0000a102` | 1/1/0 | `dsc=0x48e2a040 buf=0x48c74de0 status=00000008` | 32 |
| **3** | **40** | **`8000000c`** | **`0000000c`** | **0** | **0** | **`idmac`** | `0000a102` | 1/1/0 | `dsc=0x48e2a040 buf=0x48c74de8 status=00000008` | **40** |
| 4 | 36 | `0000000c` | `0000000c` | 0 | 0 | `idmac` | `0000a102` | 1/1/0 | `dsc=0x48e2a040 buf=0x48c74de4 status=00000008` | 36 |

Sequence 3 is the previously failing transfer: `arg=17efb428 fn=1 incr=1
addr=1f7da bytes=40`, the CMD53 block carrying the 38-byte MAC-report payload.
Descriptor: ring `0x48e2a040`, slots 0-0, count 1, `desc64=0`,
`buf=0x48c74dc0`, `len=40`, `dma_len=40`, `map=1`, `elapsed_ns=466625`.

4 of 4 `engine=idmac`; 0 `pio-fallback`; 0 `start=-22`; 0 `own_timeout=1`; all
`err=0 pio=0`. `own_submit`/`own_dma_irq`/`own_finish` are `8000000c`
throughout, exactly as §19.1 predicts and as agreed in advance.

Against the §17.4 baseline, for that same transfer:

| Field | §17.4 baseline | §19 with 0012 |
|---|---|---|
| `own_before` | `8000000c` | `8000000c` |
| `own_after_poll` | `8000000c` | **`0000000c`** |
| `own_timeout` | 1 | **0** |
| `start` | −22 | **0** |
| Engine | `pio-fallback` | **`idmac`** |
| IDSTS at IRQ | `00000000` | **`0000a102`** |
| `wlan0` | absent | **present, UP** |

`own_before` is unchanged, which is the point: the non-invalidated read still
sees the stale OWN-set word, and only the read taken after a single 64-byte
line invalidate sees the clear. The two reads are microseconds apart on the
same address in the same poll loop, and the only thing between them is the
cache operation.

### 19.6 Classification

**Decision case 1.** `own_before = 0x8000000c`, `own_after_poll =
0x0000000c`, `start = 0` for the previously failing transfer. The descriptor
cache-coherency defect is causally confirmed: the IDMAC had released ownership
all along, and the CPU could not see it because the descriptor line was stale
in D-cache and nothing invalidated it. `dmam_alloc_coherent()` being
unavailable on this NOMMU port (§18) is the mechanism; the missing
CPU-direction sync at the OWN poll is the defect.

Classified at the descriptor boundary, as required. That `wlan0` also comes up
is corroboration, not the criterion — the criterion is the `8000000c →
0000000c` transition on the descriptor the baseline timed out on. The pattern
is reproducible: the earlier warm capture of this same image showed the
identical per-sequence pattern, only sequence 3 needing the invalidate and
sequences 1, 2 and 4 not.

Case 4 does not apply — the problematic transfer entered IDMAC **and** the
downstream layers completed. Stopping here regardless, per the experiment's
own terms.

### 19.7 What this result does not show

* It does not show that the descriptor ring is now coherent in general. The
  invalidate happens at one read site under one condition. Any other CPU read
  of a descriptor — including the IRQ and completion diagnostic reads, which
  still return `8000000c` — remains unsynchronised. A production fix should be
  a proper coherent allocator or a systematic sync at every ownership
  boundary, not this poll-site patch.
* It does not show the payload buffers are correct. Those go through
  `dma_map_single`/`dma_unmap_single` and were never part of this variable.
  The payloads happen to be right here; that is not evidence about them.
* It does not test sustained traffic, association, DHCP, throughput, or
  anything beyond the four boot-handshake CMD53 reads. `wlan0` existing and
  being UP is a link-layer fact about the interface, not about connectivity.
* It does not remove the `0010` question. This run is on the no-0010 baseline
  by design; whether `0010` is still needed once descriptors are coherent is a
  separate experiment.
* One board, one strict-cold boot plus one warm boot, one C6 firmware. The
  timing dependence of a cache-staleness bug means a negative on some other
  run would not contradict this, and a positive on four transfers is not a
  distribution.
* The `ttyGS1` root shell used to read all of this is unauthenticated access
  obtainable from a physical USB connection alone. It is diagnostic-only, for
  this isolated investigation, and must not be treated as acceptable for
  Wi-Fi/SSH acceptance or integration into `main`.

### 19.8 Housekeeping

Disk guard, executed at the build boundary: 16.27 GiB free before; removed the
superseded volume `easystick-p4-m2-no0010-idmactrace-20260812-out` after
extracting and hashing its 6.8 MB reference set; 5.42 GiB recovered; 21.69 GiB
free after. Confirmed intact: the flashed §17.3 artifact set, the staged
candidate, the stock recovery image `229459f2…020c24`, all raw serial logs,
and the git dirty state. Preserved and reported rather than deleted:
`easystick-p4-src-vol` (14 GB, mounted by the running build), the
`espressif/idf` image (C6 build, out of scope), ccache, c6-out, and several
unrelated trees of uncertain provenance. After preserving this section's
diagnostic artifacts, 18 GiB free — above the ~12 GiB guard, so no further
cleanup was performed.

Evidence files, all under `C:\Users\developer\tmp\`:

| File | Bytes | SHA-256 |
|---|---:|---|
| `easystick-p4-descinv-flash-20260812.log` | 14164 | `16c24ea750b62aacb6078ab3b7e3d9b5fa33644e863503c322505c60cd90a838` |
| `easystick-p4-descinv-strictcold-20260812.log` | 19080 | `4d9329570723c7f9e2411305b0fc226a1769d9232274e5687215f47f4b923d5d` |
| `easystick-p4-descinv-strictcold-shell-20260812.log` | 6919 | `d36d6c56f4982ef2f93c71f730d94fc03b9d131ed253ae809a7e14f2db22b70a` |

## 20. Production IDMAC descriptor-ring fix (patch 0013)

Experimental patch `0012-easystick-dw-mmc-idmac-desc-invalidate.patch` is
preserved unchanged as the §19 evidence artifact and is **not** promoted. This
section covers a separately gated production patch,
`0013-easystick-dw-mmc-idmac-noncoherent-ring.patch`, selected by
`EASYSTICK_IDMAC_NONCOHERENT_RING=1`. `build-m1.sh` rejects the combination of
0012 and 0013 outright: both rewrite the same OWN polls, so they are mutually
exclusive by construction rather than by convention.

### 20.1 What 0013 changes

The coherent path is untouched. `dmam_alloc_coherent()` is still attempted
first and still owns the ring whenever it succeeds; its call site appears in
the patch only as context. Only the failure branch changes.

Removed: the `devm_kzalloc()` + `virt_to_phys()` hand-rolled fallback. That
construction produced a DMA address the DMA API had never issued, memory the
API would not cache-maintain, and a silent dependency on a `phys == dma` 1:1
mapping.

Added: `dma_alloc_noncoherent(dev, DESC_RING_BUF_SZ, &sg_dma,
DMA_BIDIRECTIONAL, GFP_KERNEL)`, whose returned handle is the only value used
as the ring DMA address, plus explicit ownership transitions on every path that
moves the ring between the CPU and IDMAC.

`DMA_BIDIRECTIONAL` is not a conservative default here, it is the actual
direction: the CPU writes descriptors into the ring and IDMAC writes the OWN
bit back into the same memory.

### 20.2 The semantic gate, and why the first build was not flashed

The first clean build of 0013 completed successfully — exit 0, patch applied to
both kernel packages with no failed hunk and no fuzz. It was **not flashed**,
because the mandatory semantic review found a defect that no artifact gate can
see.

**Finding 1 — partial synchronisation could discard a written descriptor.**
On this architecture `arch_sync_dma_for_cpu(DMA_BIDIRECTIONAL)` resolves to
`arch_dma_cache_inv()`: a pure invalidate with no write-back
(`arch/riscv/mm/dma-noncoherent.c`, read from this kernel's own tree). A
CPU-side sync therefore *discards* whatever dirty data its range holds.

`struct idmac_desc` is 16 bytes; `dma_get_cache_alignment()` is
`ARCH_DMA_MINALIGN` = `L1_CACHE_BYTES` = 64 on this platform. **Four
descriptors share one cache line.** The first draft synchronised one descriptor
at a time using `ALIGN_DOWN(offset, dma_get_cache_alignment())`, and
`dw_mci_prepare_desc{32,64}()` interleave the per-descriptor OWN poll with the
per-descriptor write. Advancing from `desc[i]` to `desc[i+1]` therefore
invalidated a line that already held the OWN bit, buffer size and buffer
address just written into `desc[i]`, and threw them away.

That is reachable whenever a transfer needs two or more descriptors — a
multi-segment scatter list, or any transfer above `DW_MCI_DESC_DATA_LENGTH`
(4 KiB). Every §19 boot transfer is `desc_count=1`, where `ALIGN_DOWN(0, 64)`
is 0 and nothing is reached back over. A strict-cold boot would therefore have
passed while sustained Wi-Fi traffic corrupted the ring: precisely the
"boot fixed, runtime broken" outcome this gate exists to prevent.

**Finding 2 — no ownership return at completion or stop.** The only CPU-side
sync in the first draft lived inside the poll accessor, which is what forced
the unsafe per-descriptor granularity in the first place. Neither
`dw_mci_dmac_complete_dma()` nor `dw_mci_idmac_stop_dma()` returned the ring to
the CPU domain at all.

Three alternatives were considered and rejected:

* *Clean before invalidating* — unsafe. Writing the line back clobbers IDMAC's
  OWN-clear on the very descriptor being polled.
* *One descriptor per cache line* — would change `ring_size` and every `desc++`
  in the driver; far outside the stated scope.
* *Clamping the invalidate to exclude already-written descriptors* — collapses
  to an empty range, so the poll could never observe a device update.

### 20.3 The revision that was gated

Ownership is now whole-ring and explicit, with the state carried in a new
`bool sg_device_owned` beside `sg_noncoherent`:

```text
CPU-owned --ring_to_device--> device-owned --completion/stop--> CPU-owned
```

Both helpers are idempotent — a call in the direction the ring is already in
does nothing — and inert on a coherent ring, so they are safe to place on every
path that could transfer ownership.

Whole-ring granularity removes the interior boundary Finding 1 exploited. The
sync range is exactly the dedicated `DESC_RING_BUF_SZ = PAGE_SIZE` allocation,
which is page-aligned and therefore cache-line aligned at both ends, so a sync
can never reach memory outside the ring. No cache line size is hard-coded
anywhere. The one place alignment is named uses `dma_get_cache_alignment()`,
and it is a `WARN_ON_ONCE` assertion at allocation rather than an assumption.

`dw_mci_prepare_desc{32,64}()` are restructured into **poll-all-then-fill**:
pass 1 walks the scatter list with the identical `desc_len` splitting and only
polls OWN, so the ring holds no dirty CPU writes and can be refreshed from
memory as often as the poll needs; pass 2 rewinds to `desc_first` and writes
the chain with no syncs at all. The per-descriptor 10 us / 100 ms cadence and
the order in which descriptors are waited for are unchanged.

The poll accessor is now:

```c
static u32 dw_mci_idmac_own32(struct dw_mci *host, struct idmac_desc *desc)
{
	u32 val;

	dw_mci_idmac_ring_to_cpu(host);
	val = READ_ONCE(desc->des0);
	if (!IDMAC_OWN_CLR64(val))
		dw_mci_idmac_ring_to_device(host);

	return val;
}
```

which is the required protocol directly: synchronise before each CPU read, and
if OWN is still set return the memory to the device domain before waiting and
polling again. The experimental unsynchronised `own_before` read of 0012 is not
present on this path.

### 20.4 Ownership audit, path by path

| Path | Site | Transition | Result |
|---|---|---|---|
| First-use ring init | `dw_mci_idmac_init()` entry / exit | to CPU, then to device after the links are written | The trailing handover cleans the freshly written ring links before `DBADDR` is programmed, so the first CPU-side sync cannot discard them. The leading `ring_to_cpu()` exists so a stale `sg_device_owned` from resume or from `err_own_bit` cannot suppress that handover |
| Descriptor fill | `dw_mci_prepare_desc{32,64}()` pass 1 | to CPU | Ring is clean and CPU-owned for the whole poll; pass 2 writes with no sync |
| OWN poll | `dw_mci_idmac_own{32,64}()` | to CPU before each read; to device while OWN stays set | Cadence unchanged |
| Submission | `dw_mci_idmac_start_dma()` | to device | Single handover, after the complete chain is written and before the IDMAC kick |
| Normal completion | `dw_mci_dmac_complete_dma()` | to CPU | Runs before `dma_ops->cleanup()`. Inert for EDMAC and for a coherent ring |
| Stop / error / abort | `dw_mci_idmac_stop_dma()` | to CPU | Placed after the controller is disabled and software-reset, so the device cannot touch the ring afterwards. This is the only return for paths that bypass the completion callback |
| OWN timeout recovery | `err_own_bit` | to CPU before `memset()` | Without it, the invalidate inside the following `dw_mci_idmac_init()` would discard the cleared ring |
| DMA start failure | `dw_mci_idmac_start_dma()` `out:` | none required | `start` can only fail when `prepare` failed, which has already run `err_own_bit` then `init()`, leaving a consistent device-owned ring |
| Allocation lifetime | `devm_add_action_or_reset()` | — | Device lifetime: probe failure and remove only |
| Probe failure | `devm` unwind | — | `devm_add_action_or_reset()` runs the release immediately if registration itself fails; the code then clears `sg_cpu`/`sg_noncoherent` so the caller falls through to `no_dma` |
| Remove | `devm` unwind | — | Frees once, clears `sg_cpu`, `sg_noncoherent`, `sg_device_owned` |
| Runtime suspend | — | — | **Not reachable.** `dw_mci_idmac_ops` has no `.exit` member, and every `dma_ops->exit` call site including `dw_mci_runtime_suspend()` is guarded by `if (host->dma_ops->exit)`. The ring survives suspend by construction, not by luck |
| Runtime resume | `dw_mci_runtime_resume()` then `dma_ops->init()` | to CPU, then to device | Re-links and re-hands-over the same allocation; `sg_cpu`/`sg_dma` are not reallocated |

### 20.5 Post-build artifact gate

Kernel-only rebuild in the existing output volume, after
`linux-dirclean` + `linux-headers-dirclean` discarded both stale trees, so the
kernel was genuinely re-extracted and re-patched.

| Check | Expected | Observed |
|---|---|---|
| 0013 applied to `linux-custom` | clean | `patching file dw_mmc.h`, `dw_mmc.c`; no failed hunk, no fuzz, no reversal |
| 0013 applied to `linux-headers-custom` | clean | same |
| Patched `dw_mmc.c` identical in both packages | equal | `41fb5cca…46e41` in both |
| Patched `dw_mmc.h` identical in both packages | equal | `fabb1b8f…9ef13` in both |
| Emitted source reproduces the reviewed tree | equal | Both match the reviewed scratch tree byte-for-byte |
| `0012-…desc-invalidate` staged | absent | 0 |
| why2025 `0015-…cache-thunk-hardening` staged | absent | 0 |
| ESP-Hosted `0010-…boot-packet-len-poll` staged | absent | 0 |
| Forced-PIO switch compiled in | absent | 0 |
| `devm_kzalloc` on the ring | absent | 0 (the two matches are one comment and the unrelated upstream `pdata` allocation) |
| `virt_to_phys` | absent | 0 (single match is inside a comment) |
| `dma_alloc_noncoherent` / `dma_free_noncoherent` | present | 1 call each |
| `dmam_alloc_coherent` | unchanged | present, context-only in the patch |
| One-way `DMA_TO_DEVICE` sync on the ring | absent | 0 |
| `.exit` in `dw_mci_idmac_ops` | absent | 0 |
| 10 us / 100 ms poll cadence | 2 sites | 2 |
| New boot string compiled into `Image` | present | `IDMAC: noncoherent descriptor ring with explicit ownership sync`; the old `cached descriptor ring with manual cache sync` is gone |
| `linux.config` applied | `5169cacc…3adacb` | identical to §17.1 |
| `easystick-stamp-p4.dtb` | `0fb1f66a…19528d` | identical to §17.1 |
| `esp32_sdio.ko` | `8700443b…508da5` | identical to §17.1, in both the build tree and the target tree |
| `rootfs.squashfs` | §17.3 set | build output differs (it has no `ttyGS1` line), so the §17.3 rootfs already on the board is reused unchanged |
| Changed flash region | `Image` only | `Image` `c8b2cf59…b61d8e`, 6,576,968 bytes |

`checkpatch.pl --strict` on the revised patch: 0 errors, 0 warnings, 0 checks.

The byte-identical `esp32_sdio.ko` is the direct evidence that the ESP-Hosted
applied sources are unchanged; it is a stronger statement than comparing the
source tarball, because it also covers the toolchain and the kernel headers the
module was built against.

### 20.6 Candidate provenance

| Item | Value |
|---|---|
| Docker build image ID | `sha256:ffde1cf967412b9c349babe1100a148d2846d5fa68087275820b630881aee5c8` |
| Image repo digest | none — built locally, so `:latest` is not a stable identifier and the ID above is authoritative |
| Image created | 2026-08-11T00:19:36Z |
| Output volume | `easystick-p4-m2-ring0013-20260812-out` |
| Source volume | `easystick-p4-src-vol` |
| Linux commit | `acb7cf4c1184e27622be0faf89244d5001ed1e87` (v6.18.35) |
| why2025 reference | `bd790395c14968a27c00f962c9cd08d66f39de24` |
| 0013 patch SHA-256 | `cf39481791aa4c05b175a18a6811026cc257c8c0dadeaafb317dd2c4ac8dae99`, 17,056 bytes |
| Environment switches | `EASYSTICK_ESPHOSTED_DISABLE_0010=1`, `EASYSTICK_IDMAC_NONCOHERENT_RING=1`, `EASYSTICK_SDIO_FORCE_PIO=0`, `EASYSTICK_CCACHE_DIR=/ccache` |
| Build invocation | `build-m1.sh /src /out --profile m2` inside the pinned image |
| Candidate directory | `C:\Users\developer\tmp\easystick-p4-ring0013-candidate-20260812\` |

Candidate contents. Five of the six regions are byte-identical to what is
already on the board, so `Image` is the sole experimental variable; they are
present only because the flasher writes and verifies the complete region map.

| Artifact | Bytes | SHA-256 | Relation to the flashed §19 set |
|---|---:|---|---|
| `Image` | 6,576,968 | `c8b2cf59d339d084bd4b0d019d286c2c0f93aa3fb768c5240df7de6ed8b61d8e` | **changed** |
| `bootloader.bin` | 22,976 | `39a09d946c6425daa7827f581d8f03ea5b3d335156d37dc3c53c0ae1c584b912` | identical |
| `partition-table.bin` | 3,072 | `580a0ca942e6746a768724ae7bbb486dee47a228ca46b477ee598ac45738e87f` | identical |
| `boot-shim.bin` | 212,112 | `9d1a25bd9b65df26681fdc782bb230aecb48895ada2de77cef2cdbc48855c353` | identical |
| `rootfs.squashfs` | 1,822,720 | `c9d5e27237005a452ec45a52d2b838efc5ba84c672b832b2758b01f7ccbd5d01` | identical (§17.3) |
| `easystick-stamp-p4.dtb` | 3,084 | `0fb1f66abdc4dfd19d14c7543cb6acddeb99ee72f430f42cfce9218e7c19528d` | identical |

The C6 is not written. The §19 candidate set at
`C:\Users\developer\tmp\easystick-p4-descinv-candidate-20260812\` is retained
unchanged as the rollback image.

### 20.7 Predicted trace, recorded before the run

Recording the prediction before the run is what makes the trace falsifiable
rather than merely interpretable afterwards. Under 0013 the descriptor-boundary
fields should read:

| Field | Prediction | Why |
|---|---|---|
| `own_before` | OWN **cleared** | The previous transfer's completion returned the ring to the CPU domain, so the first read already sees IDMAC's cleared OWN bit. This is the visible difference from 0012, where the unsynchronised read returned `8000000c` |
| `own_after_poll` | OWN cleared | Same read, after the poll settles |
| `own_submit` | OWN **set** | Read from the CPU's own just-written descriptor, before the handover |
| `own_dma_irq` | OWN set | The 0011 diagnostic reads the cached copy inside the ISR, before the completion-path return to the CPU domain |
| `own_finish` | OWN cleared | Read after completion has invalidated the ring |

`own_dma_irq` and `own_finish` are **not** validity controls; only `own_before`
and `own_after_poll` are taken through the synchronised path.

### 20.8 What this patch and this gate still cannot see

* The 0011 diagnostic reads `des0` without synchronising
  (`dw_mci_cmd53_trace_own()`, feeding `own_dma_irq` and `own_finish`). It is a
  read, so it cannot dirty a line or corrupt the ring, but those two fields
  report the CPU's cached view.
* `sg_device_owned` is a plain `bool`, not atomic. Its transitions are
  serialised by the transfer lifecycle — the CPU does not touch the ring
  between submission and completion, and IDMAC completion and error-stop are
  mutually exclusive within one interrupt — and this adds no sharing that the
  pre-existing unlocked `memset()` and OWN poll do not already assume. It is
  not proof against a future caller that violates that lifecycle.
* A successful build and a clean patch application prove nothing about
  behaviour. Only the descriptor-boundary classification from a strict-cold run
  is evidence, and one passing run is not a reproducible baseline; three
  unchanged strict-cold boots are required before promotion.
* This remains a single-board result on one silicon revision.
* why2025 `0015-riscv-esp32p4-cache-thunk-hardening.patch` is deliberately not
  in this build. If 0013 reproduces the causal success, the cache-thunk
  hardening is the next **independent** A/B, before any sustained Wi-Fi or SSH
  traffic.

### 20.9 Housekeeping

Disk guard at the build boundary: 17.33 GiB free before the rebuild, above the
~12 GiB floor, so no cleanup was performed. A second clean-build volume would
have dropped free space to roughly 9 GiB, so the existing output volume was
reused and the kernel forced to re-extract with `linux-dirclean` /
`linux-headers-dirclean` instead of allocating a new volume or deleting
anything. 17.56 GiB free after the rebuild and after preserving the candidate.
Confirmed intact: the §19 flashed candidate set, the §17.1 reference set, the
stock recovery image, all raw serial logs, and the git dirty state.

Evidence files, all under `C:\Users\developer\tmp\`:

| File | Bytes | SHA-256 |
|---|---:|---|
| `easystick-p4-ring0013-build-clean-20260812.log` (first build, gate-rejected) | 5,679,283 | `27b20049594459cabdd4c73441da4f0434512c664fc3d66ce94e8064c0a96d5d` |
| `easystick-p4-ring0013-build2-20260812.log` (revised build, gated) | 143,519 | `1014dd214825b9b495b91e140afdbc84e513068c4e61de1a878d5f4a2de7a758` |

### 20.10 Status

Every semantic and artifact gate passes. The candidate is frozen and staged.
**Nothing has been flashed and the board has not been touched.** The next step
is one flash of `Image` only, followed by one strict-cold run under the
established ordering — watcher armed, disconnect, at least 5.000 s absent,
reconnect, 90 s passive capture with no control-line activity — classified
first at the descriptor boundary.

## 21. First strict-cold run on the 0013 production ring

Run 1 of the 3 unchanged strict-cold boots required before 0013 may be
promoted to the reproducible production baseline.

### 21.1 What was written

Only the kernel `Image` region was physically written. `flash-candidate.ps1`
writes all six regions, so it was used for its gates only and the write and
verify were issued explicitly.

| Step | Command | Result |
|---|---|---|
| Preflight | `flash-candidate.ps1 … -AllowCandidateWrite -WhatIf` | exit 0, `WhatIf: no bytes written` |
| Write | `esptool --chip esp32p4 --port COM10 --baud 460800 --before default_reset --after no_reset write_flash -z --flash_mode keep --flash_freq keep --flash_size keep 0x90000 <candidate>\Image` | erase range `0x00090000..0x006d5fff`, 6,576,968 B, `Hash of data verified`, `Staying in bootloader` |
| Verify | `esptool … verify_flash 0x90000 <candidate>\Image` | `-- verify OK (digest matched)`, 6,576,968 B @ `0x00090000` |

No `erase_flash`. No bootloader, partition table, boot shim, rootfs, DTB, NVS,
PHY data, or C6 write. The five unchanged regions were hash-confirmed against
§20.6 before the write but never programmed.

Gates: stock readback `229459f2…020c24` matched; six artifacts present, size-
gated by the script and separately hash-confirmed 6/6; esptool 4.8.1; `chip_id`
identified ESP32-P4 revision v1.3, MAC `e8:f6:0a:e2:5e:73`.

| Item | Value |
|---|---|
| Source commit | `94c61528157707f940c244093ed553b2125d513a` (local, not pushed at flash time) |
| `Image` SHA-256 | `c8b2cf59d339d084bd4b0d019d286c2c0f93aa3fb768c5240df7de6ed8b61d8e` |
| P4 MAC | `e8:f6:0a:e2:5e:73` |
| Preflight log | `easystick-p4-ring0013-preflight-20260813.log`, 756 B, `da6a12050fe935ffda5029f8d3eec6005190c37aca769b1440fa8a6905758e9b` |
| Flash log | `easystick-p4-ring0013-flash-20260813.log`, 7,573 B, `79c770efc1f6f9e3d55e2a1007cd1a32bd83a04806041b2f383cd6335476e87e` |
| Verify log | `easystick-p4-ring0013-verify-20260813.log`, 911 B, `e878c03d7af5748ce200b5142f2b08d94b06ff0a312966a19a9f3382240e6362` |

Two tooling defects were found and are recorded because both are gates that
looked healthy:

* **`flash-candidate.ps1 -WhatIf` does not work under Windows PowerShell 5.1.**
  `Get-FileHash` honours `$WhatIfPreference` there and returns `null`, so the
  script died on its own stock-readback gate at exit 1 before checking
  anything. PowerShell 7.6.3 does not, and the preflight above is the pwsh 7
  run. The `-WhatIf` path had never been exercised before.
* **`Assert-File` checks presence and size, not content.** It cannot see a
  wrong artifact of plausible size. The 6/6 hash confirmation is separate and
  external to the script.

### 21.2 Strict-cold conditions

The watcher was armed before the disconnect, per the established ordering.

| Property | Value |
|---|---|
| Armed | 2026-08-13T07:12:23.530+09:00 |
| Detached | 2026-08-12T22:16:12.069Z |
| Returned | 2026-08-12T22:16:26.281Z |
| Absence | **14.212 s** (gate: at least 5.000 s) |
| Passive capture | 90.112 s, 19,058 bytes, first byte at 0.028 s |
| `control_lines_driven` | false |
| `reset_requested` | false |
| `port_opens` | 1 |

Boot cause `rst:0x17 (CHIP_USB_UART_RESET)`, `boot:0x20c
(SPI_FAST_FLASH_BOOT)`. The kernel banner is `#1 Wed Aug 12 13:35:35 UTC 2026`,
the 0013 build. `dw_mmc 50083000.mmc: IDMAC: noncoherent descriptor ring with
explicit ownership sync` is present, and it is preceded by `coherent DMA
allocations not supported on this platform.` — so the coherent path failed as
it always has on this platform and the new noncoherent fallback is the code
actually under test, not a dormant branch.

### 21.3 Descriptor-boundary classification

This is the primary classification. All four boot CMD53 receives, from
`/sys/bus/platform/devices/50083000.mmc/cmd53_rx_trace`:

| seq | addr | bytes | start | engine | own_timeout | IDMAC completion | done bytes | err | pio |
|---:|---|---:|---:|---|---:|---|---:|---:|---:|
| 1 | `0x1f7d8` | 40 | 0 | idmac | 0 | `idsts=0000a102 dto=1 dma=1 dataerr=0` | 40 | 0 | 0 |
| 2 | `0x1f7e0` | 32 | 0 | idmac | 0 | `idsts=0000a102 dto=1 dma=1 dataerr=0` | 32 | 0 | 0 |
| 3 | `0x1f7da` | 40 | 0 | idmac | 0 | `idsts=0000a102 dto=1 dma=1 dataerr=0` | 40 | 0 | 0 |
| 4 | `0x1f7dc` | 36 | 0 | idmac | 0 | `idsts=0000a102 dto=1 dma=1 dataerr=0` | 36 | 0 | 0 |

| Criterion | Required | Observed |
|---|---|---|
| `start=-22` | zero | 0 — every transfer `start=0` |
| `pio-fallback` | zero | 0 — every transfer `pio=0`, `engine=idmac` |
| `own_timeout=1` | zero | 0 |
| Previously failing 38/40-byte RX | `start=0`, `engine=idmac` | seq 3, `start=0`, `engine=idmac` |
| IDMAC completion | present | all four, `dto=1 dma=1 dataerr=0` |
| `bytes_xfered` | correct | 40/32/40/36, matching each request |
| `err` | 0 | 0 on all four |
| `pio` | 0 | 0 on all four |

Every transfer is `desc_count=1`, ring `0x48e53000`, slots 0-0. The ring
address is stable across all four, so the `devm` lifetime did not reallocate
it.

### 21.4 Predicted against observed

§20.7 recorded the predicted OWN fields before the run. Field order is
`own_before/own_after_poll/own_submit/own_dma_irq/own_finish`.

| seq | 0012 (§19) | 0013 (this run) |
|---:|---|---|
| 1 | `00000000/00000000/8000000c/8000000c/8000000c` | `00000000/00000000/8000000c/8000000c/0000000c` |
| 2 | `0000000c/0000000c/8000000c/8000000c/8000000c` | `0000000c/0000000c/8000000c/8000000c/0000000c` |
| 3 | **`8000000c`**`/0000000c/8000000c/8000000c/8000000c` | **`0000000c`**`/0000000c/8000000c/8000000c/0000000c` |
| 4 | `0000000c/0000000c/8000000c/8000000c/8000000c` | `0000000c/0000000c/8000000c/8000000c/0000000c` |

All five predictions hold, and the two differences from 0012 are the two the
patch is supposed to cause:

* **`own_before` on seq 3.** Under 0012 the unsynchronised read returned
  `8000000c` — OWN still set in a stale cache line — and the conditional
  invalidate then corrected it by `own_after_poll`. Under 0013 the read is
  preceded by `ring_to_cpu()`, so the stale value is never observed at all.
  This was named in §20.7 as the visible difference, before the run.
* **`own_finish` on all four.** 0012 reported `8000000c` throughout; 0013
  reports `0000000c`. The 0011 diagnostic's finish read is still
  unsynchronised, but `dw_mci_dmac_complete_dma()` now returns the ring to the
  CPU domain before it runs, so the diagnostic happens to see the true value.
  This corroborates the completion-path transition; per §20.8 it is not a
  validity control, because the read itself is not synchronised.

`own_before` on seq 1 is `00000000` rather than the `0000000c` seen on later
transfers, and that is the ring-initialisation value: `dw_mci_idmac_init()`
writes `des3` and `des1` in its loop and sets `des0` only on the last
descriptor (`IDMAC_DES0_ER`), so descriptor 0 keeps the `memset` zero until the
first `prepare` writes `FD|LD` into it. The CPU therefore reads back its own
initialisation on the first transfer and IDMAC's cleared OWN on every
subsequent one, which is what the first-use handover in §20.4 is for.

`dmesg | grep -E 'WARNING|BUG|Call Trace|dma_sync|swiotlb'` returned nothing.
The `WARN_ON_ONCE` alignment assertion at allocation did not fire, so
`dma_alloc_noncoherent()` returned a handle aligned to
`dma_get_cache_alignment()`.

### 21.5 Downstream evidence

Recorded as acceptance signals only, after the descriptor boundary.

| Signal | Observed |
|---|---|
| Command `0x1` | `len=32`, `… 01 02 00 00` |
| Following RX | `len=38`, `… 03 02 06 00` + `10 bd a3 9e 22 f4` (C6 MAC) |
| Command `0xF` | `len=36`, `… 0f 02 04 00`, capabilities `0xd` — WLAN on SDIO, BT/BLE, HCI over SDIO, BLE only |
| `wlan0` | **present and UP**: `<BROADCAST,MULTICAST,UP,LOWER_UP> mtu 1500`, `link/ether 10:bd:a3:9e:22:f4` |
| `esp32_sdio` | Live |

The `wlan0` MAC equals the MAC delivered in the 38-byte receive, so the
interface was created from data that crossed the descriptor boundary under
test rather than from a default.

`esp32_sdio mmc0:0001:2: probe with driver esp32_sdio failed with error -22`
appears in this capture. It is **not** a regression: the same line appears in
the §19 0012 strict-cold log. It is the SDIO function-2 probe, not the IDMAC
`start=-22` this experiment classifies, and both runs reach `wlan0` UP with it
present. It is unexplained and out of scope here.

### 21.6 What this run does not show

* One run. Two more unchanged strict-cold boots are required before 0013 is a
  reproducible baseline. A cache-staleness defect is timing-dependent, so a
  single pass is weak evidence by exactly the argument §19.7 made.
* Every transfer here is `desc_count=1`. The multi-descriptor path — the one
  whose defect caused the first 0013 build to be rejected — is **not exercised
  by boot traffic at all**. Nothing in this run tests it. Only sustained
  traffic above 4 KiB or a multi-segment scatter list will, and that is
  deliberately after the cache-thunk A/B.
* `wlan0` UP is a link-layer fact about the interface. No association, DHCP,
  throughput or sustained traffic was attempted.
* The function-2 `-22` remains unexplained.
* One board, one silicon revision, one C6 firmware.
* The `ttyGS1` shell used to read this is unauthenticated root over USB. It is
  diagnostic-only for this investigation and must not survive into Wi-Fi/SSH
  acceptance or `main`.

### 21.7 Evidence

| File | Bytes | SHA-256 |
|---|---:|---|
| `easystick-p4-ring0013-strictcold-20260813.log` | 19,058 | `7873c750aff32ed7796a030ab32edaaa4433529ee90b8678a262d621b897fa6a` |
| `easystick-p4-ring0013-strictcold-20260813.json` | 382 | `4c2d35b39cbb1dd2919510885de7ad1303acbb84d6f6f7f74e5acbd17f8763d5` |
| `easystick-p4-ring0013-strictcold-shell-20260813.log` | 6,896 | `dda7619148fe594be781aeb13e533a4a0e943591858984e6ad3d7d8562f993c7` |

### 21.8 Status

Run 1 of 3: **pass at the descriptor boundary**, with the pre-registered trace
prediction confirmed. 0013 is not yet promoted. Next is a second unchanged
strict-cold boot of the identical image, then a third. why2025
`0015-riscv-esp32p4-cache-thunk-hardening.patch` is still deliberately absent,
and remains the next independent A/B after promotion, before any sustained
network testing. No source, image or runtime state was changed during or after
this run.

## 22. Second strict-cold run, and what independent verification found

Run 2 of the 3 unchanged strict-cold boots. No flash, no source change, no
module reload, no SDIO or C6 reset, no interface change, no association, no
DHCP. The board carried the identical `Image`
`c8b2cf59d339d084bd4b0d019d286c2c0f93aa3fb768c5240df7de6ed8b61d8e` written in
§21.1 and was not written again.

Before the run, `94c6152` and `f223a8f` were pushed to
`origin/agent/dma-no0010-ab-evidence`
(`f223a8fe06ad8da8136b8b55d5c656c3c10b3d2a`) with `--no-recurse-submodules`, so
the frozen source candidate and the Run 1 evidence exist off this machine. The
dirty `vendor/linux` submodule and untracked `tools/pdf-schematic-text/` were
excluded and remain local.

### 22.1 Strict-cold conditions

| Property | Run 1 | Run 2 |
|---|---|---|
| Armed | 2026-08-13T07:12:23.530+09:00 | 2026-08-13T07:33:06.994+09:00 |
| COM10 present at arm | esptool `verify_flash` on COM10 exited 0 moments before | `GetPortNames()` check immediately before |
| Detached | 2026-08-12T22:16:12.069Z | 2026-08-12T22:35:21.421Z |
| Returned | 2026-08-12T22:16:26.281Z | 2026-08-12T22:35:33.469Z |
| Absence | 14.212 s | **12.048 s** (gate 5.000 s) |
| Passive capture | 90.112 s, 19,058 B | 90.069 s, 19,056 B |
| First byte | 0.028 s | 0.027 s |
| `port_opens` | 1 | 1 |
| C6 last reset cause | `POWERON_RESET` | `POWERON_RESET` |

The arm timestamp is console output, not a field in either evidence file. It is
transcribed here because the watcher writes only the serial byte stream to its
`-OutFile`. The separate check that COM10 was present immediately before arming
is what makes the detach edge a real present-to-absent transition; without it,
an already-absent port would let the watcher stamp the arm instant as a
fabricated detach. That check was run for both runs and is recorded here for
the same reason.

### 22.2 Descriptor-boundary classification

All four boot CMD53 receives, from `cmd53_rx_trace`:

| seq | addr | bytes | start | engine | own_timeout | IDMAC completion | done bytes | err | pio |
|---:|---|---:|---:|---|---:|---|---:|---:|---:|
| 1 | `0x1f7d8` | 40 | 0 | idmac | 0 | `idsts=0000a102 dto=1 dma=1 dataerr=0` | 40 | 0 | 0 |
| 2 | `0x1f7e0` | 32 | 0 | idmac | 0 | `idsts=0000a102 dto=1 dma=1 dataerr=0` | 32 | 0 | 0 |
| 3 | `0x1f7da` | 40 | 0 | idmac | 0 | `idsts=0000a102 dto=1 dma=1 dataerr=0` | 40 | 0 | 0 |
| 4 | `0x1f7dc` | 36 | 0 | idmac | 0 | `idsts=0000a102 dto=1 dma=1 dataerr=0` | 36 | 0 | 0 |

`start=-22`, `own_timeout=1` and `pio=1` occur zero times in the file. Every
transfer is `desc_count=1`, ring `0x48e53000`, slots 0-0, as in Run 1.

Criterion 5 was corroborated twice **without** using the printed `bytes=` field,
because that field and the `done[]` count share a source:

* Decoding the CMD53 argument, bits 8:0 as the byte count and bits 25:9 as the
  address: `0x17efb028` -> `1f7d8`/40, `0x17efc020` -> `1f7e0`/32,
  `0x17efb428` -> `1f7da`/40, `0x17efb824` -> `1f7dc`/36. All four also decode
  `rw=0`, `fn=1`, `incr=1`.
* Subtracting the buffer VA from the `done[]` completion pointer:
  `0x48c74968-0x48c74940`=40, `0x48c74de0-0x48c74dc0`=32,
  `0x48c74de8-0x48c74dc0`=40, `0x48c74de4-0x48c74dc0`=36.

The raw status words carry no error bits: `data_status=00000008` (DTO only) on
all four; RINTSTS `0x2c`/`0x3c` set only CD, DTO, RXDR and TXDR, with no RE,
RCRC, DCRC, RTO, DRTO, HTO, FRUN, HLE, SBE or EBE; IDSTS `0xa102` sets RI and
NIS with AIS, FBE, DU and CES all clear.

Trace completeness was checked rather than assumed. `DW_MCI_CMD53_TRACE_DEPTH`
is 8 and four slots were used, so there is no ring wrap and no record dropped
by the `valid_seq != seq` skip; the printed sequence numbers are contiguous
1-4; and the shell prompt returns immediately after record 4, so the output is
not truncated.

Correspondence between the two evidence files was checked in the direction that
could expose a missing transfer: the driver's own `BOOT_CMD53_RX: len=` lines
report 40, 32, 38 and 36, and the third maps to trace seq 3 at 40 bytes because
the host rounds the 38-byte payload up to the bus transfer size. The header at
that receive gives `0x1a + 0x0c` = 26 + 12 = 38, which is the same arithmetic
§19 used.

### 22.3 The OWN signature, and what actually discriminates 0013

| seq | Run 1 | Run 2 |
|---:|---|---|
| 1 | `00000000/00000000/8000000c/8000000c/0000000c` | `00000000/00000000/8000000c/8000000c/0000000c` |
| 2 | `0000000c/0000000c/8000000c/8000000c/0000000c` | `0000000c/0000000c/8000000c/8000000c/0000000c` |
| 3 | `0000000c/0000000c/8000000c/8000000c/0000000c` | `0000000c/0000000c/8000000c/8000000c/0000000c` |
| 4 | `0000000c/0000000c/8000000c/8000000c/0000000c` | `0000000c/0000000c/8000000c/8000000c/0000000c` |

Identical. `des0` `0x0000000c` is `FD|LD` with OWN clear.

**The eight acceptance criteria do not, by themselves, distinguish 0013 from
0012.** §19.5 records that 0012 - the experimental patch that was deliberately
not promoted - already met all eight: 4 of 4 `engine=idmac`, zero
`pio-fallback`, zero `start=-22`, zero `own_timeout=1`, `err=0` and `pio=0`
throughout, bytes 40/32/40/36. A pass on the criteria is therefore evidence
that the boundary works, not evidence about which patch made it work.

What discriminates is the `own_before` field on seq 3: `8000000c` under 0012,
where the unsynchronised read returned a stale line that the conditional
invalidate then corrected, against `0000000c` under 0013, where the read is
preceded by `ring_to_cpu()` and the stale value is never observed. That
difference was pre-registered in §20.7 before Run 1 and has now held twice.
Any future statement that a run "proves 0013" must cite this field, not the
criteria matrix.

### 22.4 Independent verification of Run 2

Run 2 was classified by four independent read-only passes over the evidence -
criteria re-derivation, Run 1/Run 2 regression diff, anomaly hunt, and
protocol audit - followed by an adversarial attempt to refute the result. All
four classifiers returned PASS. The adversarial pass returned **refuted**, and
it was right to: it refuted the *wording* of the claim, not the data.

Adversarial coverage was partial. Three refutation angles were dispatched -
data, method, scope - and only the scope angle completed; the data and method
angles both died on a session quota. Per §14.2, that is stated rather than
rounded up: the criteria arithmetic was independently re-derived twice by the
classifier passes, but only one of three planned refutation angles ran.

Four qualifications came out of it, none of which contradicts the run:

1. **The criteria do not attribute the pass to 0013.** Covered in §22.3.
2. **A harness parameter changed between runs.** The read-only collector's
   passive-listen window was 6 s in Run 1 and 8 s in Run 2. Both were supplied
   by hand and neither is the script default of 12 s. It is outside the causal
   path - the trace is fixed in the kernel ring at boot, and the collector
   opens a second port only after the 90 s capture has closed - but "unchanged"
   was the word used, and it was not strictly true. Run 3 uses 8 s; the series
   is recorded as 6/8/8 rather than presented as identical.
3. **Two JSON fields are declarations, not measurements.**
   `control_lines_driven` and `reset_requested` are initialised `false` at
   script start and never reassigned, so they would read `false` even if the
   lines had been driven. The evidence for those two properties is source
   inspection: the watcher contains no `.Write` call at all, and both scripts
   set `DtrEnable` and `RtsEnable` false before every `Open()` with no
   assignment to `$true` anywhere. `port_opens` and `first_byte_seconds` are
   genuinely counted and measured. This is §14.16's defect in miniature - a
   field that looks like an observation and is actually a restatement of
   intent.
4. **The ROM boot word changed and is not decoded.** Run 1 latched
   `boot:0x20c`, Run 2 `boot:0xc`; both decode `SPI_FAST_FLASH_BOOT` and
   nothing downstream differs. Bit 9 is decoded nowhere in the evidence, so
   calling the change immaterial would be a claim the evidence cannot support.
   A prediction is registered instead, before Run 3: Run 1 was the first boot
   after esptool left the chip in ROM bootloader with `--after no_reset`, while
   Runs 2 and 3 both follow an ordinary Linux session, so **Run 3 should latch
   `0xc`**. If it latches `0x20c` the explanation is wrong and the bit needs a
   real decode.

### 22.5 Reproducibility, measured rather than asserted

The two boot captures are 329 lines each and, after normalising the ESP-IDF
millisecond timestamps, differ in exactly two places: the boot word above, and
one interleaving swap between a `tx_process` line and two `read_packet len=0`
lines. The kernel banner, boot-shim version and ELF SHA-256, kernel and rootfs
byte counts, partition and segment tables, `/proc/cmdline`, the `esp32_sdio`
module size and load address `0x48ef0000`, the `wlan0` MAC, the ring base, all
four CMD53 argument/address/byte triples and every OWN, RINTSTS, MINTSTS, IDSTS
and BMOD field are identical.

One difference is worth more than its size. **The RX payload buffer moved.**
Run 1 used `va=dma=0x48c75000` for seq 2-4, which is page-aligned; Run 2 used
`0x48c74dc0`, which is 64-byte aligned but sits at a different offset in the
line grid. `0x48c74dc0` is the placement the §19 0012 strict-cold run used, and
its four completion pointers - `0x48c74968`, `0x48c74de0`, `0x48c74de8`,
`0x48c74de4` - are identical to Run 2's, so of the three strict-cold runs Run 1
is the one that differs. The same pass was therefore obtained from a
differently placed payload buffer, so it is not an artifact of one lucky
address.

The descriptor ring base did change from the §19 run, and by design: 0012 ran
on `0x48e2a040`, a `devm_kzalloc()` pointer put through `virt_to_phys()`, while
0013 runs on `0x48e53000`, a handle returned by `dma_alloc_noncoherent()`. The
new base is page-aligned, which is what §20.3 requires for whole-ring
synchronisation to be unable to reach memory outside the ring, and it is
identical across Runs 1 and 2, so the `devm` lifetime did not reallocate it.

### 22.6 Downstream evidence

Secondary, recorded after the descriptor boundary. Command `0x1` (len=32), the
following RX (len=38, C6 MAC `10 bd a3 9e 22 f4`), command `0xF` (len=36,
capabilities `0xd`), `wlan0` present and UP with `link/ether 10:bd:a3:9e:22:f4`,
`esp32_sdio` Live. The interface MAC again equals the MAC delivered across the
boundary under test. `dmesg | grep -E 'WARNING|BUG|Call Trace|dma_sync|swiotlb'`
returned nothing.

The `esp32_sdio mmc0:0001:2: probe with driver esp32_sdio failed with error
-22` line is present, as in Run 1 and in the §19 0012 run. Still unexplained,
still out of scope, still not the IDMAC `start=-22` being classified.

### 22.7 What Run 2 does not show

Beyond §21.6, which still applies in full, the verification passes surfaced
four coverage limits that were not previously written down:

* **The bus never leaves 400 kHz.** The capture contains exactly one `Bus
  speed` line, at the identification clock, and the card is announced with no
  speed qualifier. Every result here is obtained under the most forgiving
  timing the hardware offers, roughly 1.5 orders of magnitude slower than
  production SDIO. Cache-coherency defects are timing-sensitive by nature.
* **The TX path through the same ring is unobserved.** Three 512-byte CMD53
  writes and the OPEN_DATA_PATH write crossed the ring during boot;
  `cmd53_rx_trace` is RX-only. All eight criteria are RX-only, so a ring
  ownership defect on the write path - same helpers, larger transfer, opposite
  direction - would not appear.
* **No RX buffer ends on a cache-line boundary.** Buffers start line-aligned
  but run 40, 32, 40 and 36 bytes, so the tail of each buffer's final line
  belongs to memory the transfer does not own. That is the payload buffer, not
  the descriptor ring, and it is untested here.
* **`idle=poll` does not do anything on this kernel.** It is rejected at parse
  time and handed to userspace as an environment variable. Any reasoning that
  assumed it keeps the CPU out of idle states - which is exactly what changes
  cache residency and DMA race windows - is unsupported.

And the limit that governs everything after this: **every transfer in Runs 1
and 2 is `desc_count=1`.** Boot RX is at most 40 bytes and
`DW_MCI_DESC_DATA_LENGTH` is 4096, so ordinary boot traffic can never produce a
second descriptor. Repeating this boot any number of times cannot exercise the
multi-descriptor path whose defect caused the first 0013 build to be rejected
in §20.2.

### 22.8 Evidence

| File | Bytes | SHA-256 |
|---|---:|---|
| `easystick-p4-ring0013-strictcold-run2-20260813.log` | 19,056 | `55c46e86a8ee27bd9203d4674992ad4f49a26ce51be0dd034ea54ff88190633a` |
| `easystick-p4-ring0013-strictcold-run2-20260813.json` | 382 | `905b7b2a7c19c92361b0b80681ee04a2bec9e23edee7ba26aba259620e4bbf3b` |
| `easystick-p4-ring0013-strictcold-shell-run2-20260813.log` | 6,896 | `47025aec0fecbd774e98176d9982c82044342b79758a3180f9da032b8ed98f1c` |

Both serial captures contain CR bytes - 329 and 85 respectively - because they
are raw console output and the device emits CRLF. Run 1's captures carry the
same counts. Per §14.22 a CR match is a lead, not a defect; here it is the
expected content of a serial log and not a line-ending corruption.

### 22.9 Status

Run 2 of 3: **pass at the descriptor boundary**, with four qualifications
recorded above and one of three adversarial angles unrun. Nothing was changed
before, during or after the run. Run 3 is the identical image under the
identical protocol; at 3 of 3 the classification is **reproducible
single-descriptor cold-boot baseline**, which is not a claim that the IDMAC
ring implementation is validated.

## 23. Third strict-cold run and the 3-of-3 result

Run 3 of 3 on the unchanged 0013 image
`c8b2cf59d339d084bd4b0d019d286c2c0f93aa3fb768c5240df7de6ed8b61d8e`. Nothing was
reflashed between Runs 1, 2 and 3, and no source, module, SDIO, C6, network or
runtime state was changed after Run 2's verification closed.

### 23.1 Strict-cold conditions

The watcher was armed before the disconnect in all three runs. Run 3 used the
identical collector parameters as Run 2, so between Run 2 and Run 3 the entire
instrument was unchanged as well as the image.

| Property | Run 1 | Run 2 | Run 3 |
|---|---|---|---|
| Armed (Asia/Tokyo) | 07:12:23.530 | 07:33:06.994 | 12:20:51.570 |
| Detached (UTC) | 2026-08-12T22:16:12.069 | 2026-08-12T22:35:21.421 | 2026-08-13T03:21:51.026 |
| Returned (UTC) | 2026-08-12T22:16:26.281 | 2026-08-12T22:35:33.469 | 2026-08-13T03:22:04.164 |
| Absence (gate: >= 5.000 s) | **14.212 s** | **12.048 s** | **13.138 s** |
| `accepted` | true | true | true |
| `control_lines_driven` | false | false | false |
| `reset_requested` | false | false | false |
| `port_opens` | 1 | 1 | 1 |
| Passive capture | 90.112 s | 90.069 s | 90.105 s |
| Bytes captured | 19,058 | 19,056 | 19,056 |
| First byte | 0.028 s | 0.027 s | 0.023 s |
| Collector passive-listen | 6 s | 8 s | 8 s |
| ROM boot word | `0x20c` | `0xc` | `0xc` |
| C6 `POWERON_RESET` | present | present | present |

§22.4 declined to call the Run 1 -> Run 2 boot-word change immaterial, because
bit 9 of that word is undecoded, and registered a prediction instead: if the
change is the strap latch settling after the first cold cycle of the session
rather than anything about the transfer path, Run 3 should read `0xc`. **It
reads `0xc`.** That is one confirmed prediction, not a decode of bit 9, and the
bit remains undecoded.

The two-byte difference between Run 1's capture and the other two is the same
fact measured a second way. Diffing Run 1 against Run 3 line by line yields 14
differing lines and nothing else: eight second-stage bootloader millisecond
timestamps, four `read_packet: BOOT_CMD53_RX: len=0` lines that swap order
against a `tx_process` line, and the one boot-word line. `boot:0x20c` is two
characters longer than `boot:0xc`, which is the whole of the 19,058 vs 19,056
difference.

### 23.2 Descriptor-boundary classification

This is the primary classification, applied with the criteria fixed before the
run and not modified after seeing it. All four boot CMD53 receives, from
`/sys/bus/platform/devices/50083000.mmc/cmd53_rx_trace`:

| seq | addr | bytes | start | engine | own_timeout | IDMAC completion | done bytes | err | pio |
|---:|---|---:|---:|---|---:|---|---:|---:|---:|
| 1 | `0x1f7d8` | 40 | 0 | idmac | 0 | `idsts=0000a102 dto=1 dma=1 dataerr=0` | 40 | 0 | 0 |
| 2 | `0x1f7e0` | 32 | 0 | idmac | 0 | `idsts=0000a102 dto=1 dma=1 dataerr=0` | 32 | 0 | 0 |
| 3 | `0x1f7da` | 40 | 0 | idmac | 0 | `idsts=0000a102 dto=1 dma=1 dataerr=0` | 40 | 0 | 0 |
| 4 | `0x1f7dc` | 36 | 0 | idmac | 0 | `idsts=0000a102 dto=1 dma=1 dataerr=0` | 36 | 0 | 0 |

Against the acceptance list as it was written:

| # | Required | Observed in Run 3 |
|---:|---|---|
| 1 | Strict-cold absence >= 5.000 s | 13.138 s |
| 2 | `control_lines_driven=false` | false |
| 3 | `reset_requested=false` | false |
| 4 | C6 `POWERON_RESET` | present |
| 5 | All four boot CMD53 `start=0` | 4/4 `start=0`; zero `start=-22` |
| 6 | 4/4 `engine=idmac` | 4/4 |
| 7 | `own_timeout=0` | 4/4 |
| 8 | pio-fallback zero | zero; `pio=0` on all four, no `engine=pio` |
| 9 | IDMAC completion `dto=1 dma=1 dataerr=0` | 4/4, `idsts=0000a102` |
| 10 | `bytes_xfered = 40/32/40/36` | 40/32/40/36 |
| 11 | `err=0` | 4/4 |
| 12 | `pio=0` | 4/4 |
| 13 | seq 3's 40-byte RX normal | `start=0 engine=idmac own_timeout=0 err=0 pio=0`, 40 bytes |
| 14 | OWN signature not contradicting 0013 | see §23.3 |
| 15 | Command `0x1` -> 38-byte RX -> command `0xF` | all three present, in order |
| 16 | `wlan0` as secondary evidence | UP, MAC `10:bd:a3:9e:22:f4` |

Every transfer is `desc_count=1`, ring `0x48e53000`, slots 0-0 - the same ring
base as Runs 1 and 2, so the `devm` lifetime did not reallocate it across three
cold boots.

Criteria 5 and 10 were corroborated twice by routes that do not read the
printed `bytes=` field, exactly as in §22.2. The CMD53 argument carries the byte
count in bits 8:0 and the register address in bits 25:9: `0x17efb028` -> 40 at
`0x1f7d8`, `0x17efc020` -> 32 at `0x1f7e0`, `0x17efb428` -> 40 at `0x1f7da`,
`0x17efb824` -> 36 at `0x1f7dc`. Independently, each completion's buffer pointer
minus that transfer's buffer VA is 40, 32, 40 and 36. Four transfers, three
agreeing statements each.

The trace holds 4 records against `DW_MCI_CMD53_TRACE_DEPTH 8`, seq 1 to 4
contiguous, with the shell prompt returning after record 4 - so the ring was not
truncated and no transfer was silently dropped from the window. Greps for
`start=-22`, `own_timeout=1`, `pio=1`, `engine=pio`, `dataerr=1` and any nonzero
`err=` return zero across the whole capture.

Also `esp32_sdio: read_packet: BOOT_CMD53_RX: len=38` in dmesg against
`bytes=40` in the trace for seq 3: these are the same transfer seen at two
layers. The C6 delivers a 38-byte payload; the host rounds the CMD53 up to a
4-byte multiple, so the descriptor moves 40 bytes. The CMD53 argument decode
above confirms 40 was what the controller was asked for, independently of both
printed numbers. The correspondence between the two layers is by ordering, not
by a shared identifier.

### 23.3 The OWN signature

Field order is `own_before/own_after_poll/own_submit/own_dma_irq/own_finish`.

| seq | Run 1 | Run 2 | Run 3 |
|---:|---|---|---|
| 1 | `00000000/00000000/8000000c/8000000c/0000000c` | identical | identical |
| 2 | `0000000c/0000000c/8000000c/8000000c/0000000c` | identical | identical |
| 3 | `0000000c/0000000c/8000000c/8000000c/0000000c` | identical | identical |
| 4 | `0000000c/0000000c/8000000c/8000000c/0000000c` | identical | identical |

Twenty quintuples, byte-identical across three cold boots. The discriminator
against 0012 remains the one named in §20.7 before any of these runs and
restated in §22.3: seq 3's `own_before` reads `8000000c` under 0012 - a stale
OWN bit in an unsynchronised cache line, corrected only by the conditional
invalidate at `own_after_poll` - and `0000000c` under 0013, because
`ring_to_cpu()` runs before the read and the stale value is never observed. It
held three times for three.

This matters because the eight criteria of §21.3 do **not** by themselves
separate 0013 from 0012: §19.5 records 0012 meeting all eight. The OWN signature
is what makes these runs evidence about the patch rather than evidence that the
board boots.

### 23.4 What varied across the three runs

| Quantity | Run 1 | Run 2 | Run 3 | Reading |
|---|---|---|---|---|
| Image SHA-256 | `c8b2cf59…b61d8e` | same | same | not reflashed |
| Descriptor ring base | `0x48e53000` | same | same | stable allocation |
| `desc_count` | 1 | 1 | 1 | boot RX can never exceed one descriptor |
| RX payload buffer VA | `0x48c75000` | `0x48c74dc0` | `0x48c75000` | allocator placement varies run to run |
| seq 1 buffer VA | `0x48c74940` | `0x48c74940` | `0x48c74940` | first RX lands identically |
| `elapsed_ns` seq 1-4 | 487125/449938/468375/447875 | 488750/440875/471188/450062 | 489000/445563/466813/449250 | spread at most 2.1 % per seq, no outlier |
| OWN quintuples | as above | identical | identical | the classification-relevant state |

The buffer VA moving between runs is the useful part: Run 2 placed the three
later receives at `0x48c74dc0` and Runs 1 and 3 placed them at `0x48c75000`, so
the result is not an artifact of one particular address happening to sit clear
of a dirty cache line. It is a weak form of variation - two addresses, both
page- or 64-byte-aligned - and it is not a substitute for the deliberate
misalignment the multi-descriptor experiment will introduce.

### 23.5 Downstream evidence

Recorded after the descriptor boundary, as acceptance signal only.

| Signal | Run 3 |
|---|---|
| Command `0x1` | `len=32`, `… 01 02 00 00` |
| Following RX | `len=38`, `… 03 02 06 00` + `10 bd a3 9e 22 f4` |
| Command `0xF` | `len=36`, `… 0f 02 04 00`, `Capabilities: 0xd` - WLAN on SDIO, BT/BLE, HCI over SDIO, BLE only |
| `wlan0` | `<BROADCAST,MULTICAST,UP,LOWER_UP> mtu 1500`, `link/ether 10:bd:a3:9e:22:f4` |
| `esp32_sdio` | Live at `0x48ef0000` |
| `dmesg \| grep -E 'WARNING\|BUG\|Call Trace\|dma_sync\|swiotlb'` | empty |

The `wlan0` MAC again equals the MAC delivered in the 38-byte receive, so the
interface was built from bytes that crossed the descriptor boundary under test.

`esp32_sdio mmc0:0001:2: probe with driver esp32_sdio failed with error -22` is
present, as in Runs 1 and 2 and as in the §19 0012 run. It is the SDIO
function-2 probe and not the IDMAC `start=-22` this experiment classifies. It is
unexplained and deliberately out of scope until the multi-descriptor work is
done.

### 23.6 What 3 of 3 does and does not license

The result is: **the unchanged 0013 image is a reproducible single-descriptor
cold-boot baseline, 3 of 3.** That phrase is the whole claim.

It is not "0013 is validated", and it is not "production IDMAC is complete".
The limits below are the same ones §21.6 and §22.7 recorded, unchanged by
repetition, because repeating an experiment three times sharpens its variance
and does not widen its coverage:

* **Every transfer in all three runs is `desc_count=1`.** Boot RX is at most 40
  bytes and `DW_MCI_DESC_DATA_LENGTH` is 4096, so ordinary boot traffic cannot
  produce a second descriptor at all. The multi-descriptor path - whose defect
  caused the first 0013 build to be rejected in §20.2 - is untested by these
  runs and by any number of further repetitions of them. This is the next real
  gate.
* No RX buffer in any run ends on a 64-byte cache-line boundary, so the
  partial-line case that motivates the whole ownership discipline is not
  exercised at its worst point.
* The SDIO bus never leaves the 400 kHz identification clock during boot.
* The TX path issues three 512-byte CMD53 writes across the same ring, but
  `cmd53_rx_trace` is RX-only, so nothing here classifies TX.
* `wlan0` UP is a link-layer fact. No association, DHCP, SSH, throughput or
  sustained traffic has been attempted.
* One board, one silicon revision, one C6 firmware, one kernel build.
* The function-2 `-22` remains unexplained.
* The `ttyGS1` shell used to read all of this is unauthenticated root over USB.
  It is diagnostic-only for this investigation and must not survive into
  Wi-Fi/SSH acceptance or `main`.

Instrument limits carried forward from §22.4 without change: the watcher's
`control_lines_driven` and `reset_requested` fields are initialised `false` and
never reassigned, so they are declarations rather than measurements - the actual
evidence for both is that the script contains no write to either line and issues
no reset request, which was confirmed by reading it. The armed timestamp is
written to the console only and is not in the JSON.

### 23.7 Evidence

| File | Bytes | SHA-256 |
|---|---:|---|
| `easystick-p4-ring0013-strictcold-run3-20260813.log` | 19,056 | `dec75ea3166a4c8f8d161eb3fa8a01852afcb1207cdbcc641fe4f54d556b46ff` |
| `easystick-p4-ring0013-strictcold-run3-20260813.json` | 382 | `f2ec5c72600880940c254f7e086f8d0051f37220b2d0af44bdc8aa98d529398f` |
| `easystick-p4-ring0013-strictcold-shell-run3-20260813.log` | 6,896 | `942c6c086caffeb639ea8da75704295fcb2b3db114bec9b1a16f76a3d4a2a8e8` |

CR counts 329 and 85 in the two serial captures, matching Runs 1 and 2 exactly;
per §14.22 that is the expected content of a raw console log, not a line-ending
defect.

### 23.8 Status

Run 3 of 3: **pass at the descriptor boundary**, criteria unchanged from Runs 1
and 2, with the §22.4 boot-word prediction confirmed. The 0013 image is now
classified as a **reproducible single-descriptor cold-boot baseline (3/3)** -
explicitly not fully validated production IDMAC.

Nothing follows this run until the multi-descriptor experiment is designed.
why2025 `0015-riscv-esp32p4-cache-thunk-hardening.patch` stays absent; no
association, DHCP, SSH or sustained traffic; the function-2 `-22` stays
uninvestigated. The next work is the design - not the build - of a separate,
default-off diagnostic that lowers only the per-descriptor chunk cap so that a
40-byte receive becomes several descriptors sharing one 64-byte cache line with
one crossing into the next, with `desc_count >= 2` as an **acceptance
condition** rather than an observation, and with 0013 itself left untouched.

## 24. Design of the multi-descriptor experiment

Design only. Nothing here has been built, compiled or flashed, and 0013 is not
modified by any of it. The board still holds `c8b2cf59…b61d8e` untouched.

This section replaces an earlier draft of §24 (commit `7ca62ed`). That draft
made two design errors and one factual error, all three corrected below and
listed in §24.10 so the correction is not silent.

How this section was verified: six read-only source investigations, plus direct
reading of `0011`, `0013`, `fs/sysfs/file.c`, `kernel/params.c`,
`drivers/mmc/core/core.c` and `drivers/mmc/core/sdio_ops.c`. The three planned
adversarial refuters and the completeness critic **did not run** - they died on
a session limit - so the load-bearing claims here were closed by a single
reader rather than by independent attack. That is weaker in one specific way:
an error shared between the draft and the check would survive. §24.10 records
which claims that applies to.

The 3/3 result of §23 closes what boot traffic can prove. Every transfer in it
is `desc_count=1`, and `DW_MCI_DESC_DATA_LENGTH` is 4096 against a 40-byte
maximum receive, so no repetition of that experiment can ever produce a second
descriptor. The ownership discipline 0013 introduces is about descriptors that
**share a cache line with a descriptor the device may be writing**, and that
situation does not arise at all in the runs so far.

### 24.1 Objective and vehicle

Force the same four boot receives to be carried by several descriptors each,
changing nothing else: same C6 firmware, same CMD53 register addresses, same
total lengths, same clock, same cold-boot protocol. The A/B then runs against
three already-recorded baseline runs rather than needing its own.

Consequence for the gate's form: the four transfers of interest happen during
SDIO probe, so a sysfs knob written after boot is too late. The gate has to be
readable by the time `dw_mci_prepare_desc32()` first runs, which means the
kernel command line.

Geometry the experiment depends on, confirmed from source rather than assumed:
`struct idmac_desc` is four `__le32` = 16 bytes (`dw_mmc.c:85-102`); the
hardware cache block is 64 bytes (`L1_CACHE_SHIFT 6`, and the why2025 baseline's
`esp32p4_cache.c` prints `block=64` at `arch_initcall`). So **four descriptors
share one cache line**, and descriptor 0 always starts a line because
`dma_alloc_noncoherent()` returns a page-aligned handle and every transfer
restarts at `desc_first = host->sg_cpu`.

### 24.2 The gate is CMD53 receive only

A new patch `0014-easystick-dw-mmc-idmac-cmd53-rx-desc-bytes.patch`, applied on
top of an unchanged 0013.

**Scope.** The cap applies to **CMD53 reads only**. TX descriptor chunking is
untouched and keeps `DW_MCI_DESC_DATA_LENGTH`. The superseded draft applied it
to every IDMAC transfer, which would have turned each 512-byte boot TX into 64
descriptors across 16 cache lines - a second, larger, *unobserved* variable on
an RX-only trace. One variable at a time; the parameter name says so.

**Parameter.** `easystick_cmd53_rx_desc_bytes`, `module_param_named(..., uint,
0444)`. `CONFIG_MMC_DW=y`, so it is addressed as
`dw_mmc.easystick_cmd53_rx_desc_bytes=8` in `bootargs`.

`0444` is chosen deliberately, and it is not cosmetic:

* `kernel/params.c:669-670` installs `param_attr_store` **only** when
  `kp->perm & (S_IWUSR|S_IWGRP|S_IWOTH)`. With `0444` the store pointer stays
  NULL, so the parameter is not runtime-writable. It remains settable on the
  command line, which is the only place we set it.
* `perm = 0` would be stricter still (`params.c:825` - no sysfs file at all),
  and is **worse for evidence**. `0444` publishes the effective value at
  `/sys/module/dw_mmc/parameters/easystick_cmd53_rx_desc_bytes`, giving a
  reading of what the kernel *actually parsed*, independent of what we believe
  we flashed into the DTB. Requiring those two to agree is the §14.16 rule.

**Predicate.** The condition is "this transfer is a CMD53 read", semantically

```c
data->mrq && data->mrq->cmd &&
data->mrq->cmd->opcode == SD_IO_RW_EXTENDED &&
(data->flags & MMC_DATA_READ)
```

but it **MUST NOT** be spelled that way, because 0011 already computes exactly
this condition and hands it to `prepare_desc` for free. `dw_mci_prepare_desc32()`
opens with

```c
struct dw_mci_cmd53_trace_record *record = dw_mci_cmd53_trace_active(host);
```

and `record` is non-NULL precisely when `dw_mci_cmd53_trace_begin()` accepted
the request, whose test is `cmd->opcode == SD_IO_RW_EXTENDED && (data->flags &
MMC_DATA_READ)` on the `cmd`/`data` pair the driver was actually handed. So the
gate is `record != NULL`, and that is strictly better than the open-coded form:

* No `data->mrq` dereference at all, so the NULL-safety question does not arise.
  (`mmc_mrq_prep()` does set `mrq->data->mrq = mrq` at `core.c:323` and
  `mrq->cmd->mrq = mrq` at `:304` for every data request, so the open-coded form
  would also be safe - but "not reachable" beats "guarded".)
* It makes cap and trace **the same condition by construction**. There can be no
  capped transfer the trace cannot see, and no traced transfer that escaped the
  cap. Two separately-written predicates could drift; one cannot.
* `trace_begin()` zeroes `cmd53_trace_active` on entry for every data command,
  so a stale record cannot leak the cap onto a non-CMD53 transfer.

**State what that coupling actually is.** `record != NULL` is not a pure
protocol predicate; it is instrumentation state, and using it ties a functional
gate to whether the diagnostic happens to be recording. That is acceptable
*here* and would not be in production code, so say it in the patch rather than
letting a reader infer a protocol test:

> The diagnostic cap deliberately uses the presence of the already-active
> CMD53-RX trace record as its eligibility predicate, so the test cannot be
> armed for a transfer that is not simultaneously observable by the acceptance
> trace.

The failure mode it introduces is that a NULL `record` silently disables the
cap. That cannot be scored as a success, because §24.6 requires `chunk=`, `len=`
and `desc_count` to be present and correct in the trace for all four records: a
transfer with no record emits no line to read them from, and a transfer with a
record carries the cap it was actually given.

**`host->cmd` MUST NOT be used as the predicate source.** It is NULL at this
point: `__dw_mci_start_request()` calls `dw_mci_submit_data()` at `dw_mmc.c:1348`
and `dw_mci_start_command()` at `:1352`, and `:400` inside the latter is the
only assignment to `host->cmd` in the file. A predicate on `host->cmd->opcode`
would be silently false for every CMD53 read - the gate would never arm, and the
run would look like a control. `dw_mci_request_end()`'s
`WARN_ON(host->cmd || host->data)` at `:1901` means it is NULL rather than
stale, so the failure is total rather than intermittent, which is the only
mercy in it.

**Single snapshot.** 0013 made `prepare_desc` two-pass: pass 1 polls one
descriptor per chunk, pass 2 fills them. The effective cap is read **once** into
a local at function entry:

```c
unsigned int cap = (record && easystick_cmd53_rx_desc_bytes) ?
                   easystick_cmd53_rx_desc_bytes : DW_MCI_DESC_DATA_LENGTH;
```

so the pre-count, pass 1 and pass 2 cannot disagree about the chain they are
counting, polling and writing. `0444` already prevents a runtime change; the
snapshot is defence against a future edit that makes it writable, and it costs
one local.

**Validation.** Accept only `4 <= value <= 0x1000` with `value % 4 == 0`;
otherwise leave the default and say so at probe. The lower bound is the 32-bit
data bus width, the upper is current behaviour. Emit the effective cap once at
probe and add it to the trace record (`chunk=`) so the evidence states its own
arm condition rather than relying on the operator's memory of what was flashed -
a third independent statement alongside the DTB and the sysfs read.

**Arming cost.** `bootargs` lives in `dts/easystick-stamp-p4.dts:69` and the DTB
is its own 64 KiB flash region at `0xf10000`, separate from the kernel at
`0x90000`. Control run and test runs share one `Image`, with only the 64 KiB DTB
rewritten between them.

### 24.3 Ring capacity: the guard and the proof of its bound

The chunking loop is `for ( ; length ; desc++)` with **no bound against
`host->ring_size`** (`dw_mmc.c:666`, same shape in `prepare_desc64`). Upstream
safety is an arithmetic identity established at probe and never re-checked:
`mmc->max_seg_size = 0x1000` equals `DW_MCI_DESC_DATA_LENGTH`, so one segment
costs exactly one descriptor, and `mmc->max_segs = host->ring_size` caps the
segment count. Lowering the chunk cap breaks that identity. At a cap of 8 a
single 4096-byte segment would need 512 descriptors and write 8 KiB into the
4 KiB ring - past the allocation, and past the range the whole-ring
`dma_sync_single_*` covers, so invisible to 0013's ownership sync as well.

**Correction to the superseded draft.** It claimed "the SDIO path does not
re-split by `max_seg_size`: `mmc_io_rw_extended()` builds one scatterlist entry
for the whole transfer." That is wrong. `mmc_io_rw_extended()` *does* read
`max_seg_size` (`sdio_ops.c:123`) and sizes `nents = DIV_ROUND_UP(left_size,
seg_size)` (`:152`). It produces one entry for the boot receives only because
they are far below 4096. What is genuinely missing is the other half:
`max_segs` is **not** applied on the SDIO path at all - `mmc_mrq_prep()`
(`core.c:311-320`) checks `blksz`, `blocks` and total bytes and never `sg_len`.
So the ceiling is enforced by `max_seg_size` alone, which is exactly the
constant the cap decouples.

The guard is therefore an **explicit pre-count**, before anything touches the
ring:

```c
required = 0;
for (i = 0; i < sg_len; i++) {
        unsigned int this = DIV_ROUND_UP(sg_dma_len(&data->sg[i]), cap);

        if (this > host->ring_size - required)   /* overflow-safe */
                return -ENOSPC;
        required += this;
}
```

The comparison is written as `this > ring_size - required` rather than
`required + this > ring_size` so the sum cannot wrap. `sg_dma_len()` is valid
here: `dw_mci_pre_dma_transfer()` has already run `dma_map_sg()` in
`dw_mci_submit_data_dma()` before `dma_ops->start` is called.

**The bound is `required <= host->ring_size`, not `ring_size - 1`.** The
superseded draft reserved the last slot because `dw_mci_idmac_init()` marks it
`IDMAC_DES0_ER`. That reservation is unnecessary, and the proof is entirely
on-disk:

* The chunking loop **assigns** `desc->des0 = OWN|DIC|CH` (`dw_mmc.c:691-693`,
  `=` not `|=`), so reaching the last slot destroys its `ER` bit anyway - the
  reservation would not preserve what it claims to preserve.
* It never writes `des3` (32-bit) or `des6`/`des7` (64-bit), so the wrap pointer
  written by init survives even on that slot.
* Termination is by `IDMAC_DES0_LD` on the last **used** descriptor
  (`:705-711`), not by `ER`; the chain is chained-mode and `DBADDR` is programmed
  once at init, and every transfer restarts at slot 0.
* Upstream already makes the last slot reachable **by design**:
  `mmc->max_segs = host->ring_size` (`:3042`), not `ring_size - 1`, against
  `max_seg_size == DW_MCI_DESC_DATA_LENGTH`. A maximal compliant request lands
  exactly on slot `ring_size - 1`.
* `ER` is read back nowhere. It appears at three lines in the whole driver: the
  `BIT(5)` definition and the two init writes.

So `ring_size - 1` would be stricter than upstream for no stated reason.
`required <= host->ring_size` it is.

**What this pre-count cannot see:** whether the DesignWare IDMAC prefetches past
the `LD` descriptor. There is no Synopsys databook in this repository. The
strongest on-disk argument is indirect - upstream makes the `ER` slot reachable
on every dw_mmc platform - and it is an argument, not a measurement. Related and
worth recording: `IDINTEN` is programmed with only `NI|RI|TI`
(`dw_mmc.c:558-559`), so `SDMMC_IDMAC_INT_DU` (descriptor unavailable) raises no
interrupt. It still **sets its bit in IDSTS**, which the trace captures in
`irq_idsts_or` and `complete_idsts`, so a runaway is not invisible - it is
merely not announced. §23's exact-match requirement on `idsts=0000a102` already
covers it.

**Do not change `mmc->max_seg_size`, `max_req_size` or `max_blk_count`.** The
pre-count is the memory-safety guard; MMC request formation stays as 0013 has
it. Lowering `max_seg_size` would re-split requests in the *core* - it is read
at `sdio_ops.c:123` - which changes the transfers themselves and would confound
the experiment with the thing it is measuring.

**Where the boundary actually sits.** esp-hosted's RX design bound is 2048
bytes, and `esp_sdio.c:419-421` explicitly anticipates and logs reads larger
than that. A 2048-byte single-SG transfer at cap=8 consumes exactly all 256
descriptors and therefore **reaches the capacity boundary**; `required = 256`
against `host->ring_size = 256` satisfies `required <= host->ring_size`, so the
guard does **not** fire. Any transfer requiring a 257th descriptor must be
rejected before ring access - single-SG, that is 2052 bytes at cap=8
(`DIV_ROUND_UP(2052, 8) = 257`). At cap=16 the same threshold is 4100 bytes,
well above anything this path is expected to carry. So the guard is a
memory-safety bound that the cap=8 arm approaches exactly, not one it is
expected to trip; if it does trip, that is designed, observable behaviour and
not a failure of the experiment (§24.8).

**The reject is not `err_own_bit`.** It is not an OWN timeout, no descriptor has
been touched, and the ring must **not** be `memset()` and re-`init()`ed. The
pre-count runs at the top of `prepare_desc`, before:

* the `dw_mci_idmac_ring_to_cpu(host)` that opens pass 1,
* any `dw_mci_idmac_own32()` call or poll,
* any advance of `desc`,
* any `record` field write - so `record` keeps `desc_first = desc_last =
  U16_MAX` and `desc_count = 0` from `trace_begin()`, which is itself a
  distinguishable signature.

Nothing has been mutated at that point (`desc_first = desc_last = desc =
host->sg_cpu` is a local assignment), so an early `return` is safe. Ring
ownership is restored by existing code: `dw_mci_submit_data_dma()` calls
`host->dma_ops->stop(host)` on start failure, and `dw_mci_idmac_stop_dma()`
calls `dw_mci_idmac_ring_to_cpu(host)` (0013). The transfer then falls back to
PIO and completes.

**The reject MUST return `-ENOSPC`, not `-EINVAL`.** 0011 records
`record->start_ret = ret` verbatim from `dma_ops->start`, and
`dw_mci_idmac_start_dma()` propagates `prepare_desc`'s return unchanged
(`dw_mmc.c:733-734`). `err_own_bit` returns `-EINVAL`. So a capacity reject
returning `-EINVAL` would print `start=-22 engine=pio-fallback` - **byte for
byte the historic IDMAC start-failure signature this entire investigation has
used to mean "the defect reproduced"**. `-ENOSPC` prints `start=-28`, which
collides with nothing. Add a `dev_warn_once()` naming the required count, the
cap and `ring_size`, so the reject is legible in the serial log without reading
sysfs at all.

### 24.4 Instrumentation that does not perturb what it measures

The trace must show the actual chain, not just its length. The masks are, over
the first `min(desc_count, 8)` descriptors, bits above the count forced to zero:
`own_before_mask`, `own_after_poll_mask`, `own_submit_mask`, `own_finish_mask`,
`fd_mask`, `ld_mask`, `ch_mask`.

The hazard is that building them could itself change the cache traffic under
test, at which point "0014 default-off is inert" stops being provable. 0013's
helpers make the rule easy to state and easy to prove:

* `ring_to_cpu()` / `ring_to_device()` early-return when the ring is already in
  the requested domain. A call in the current direction issues **no** cache
  operation.
* `dw_mci_idmac_own32()` calls `ring_to_cpu()`, reads, and calls
  `ring_to_device()` **only if OWN is still set**.
* Pass 1 opens with one `dw_mci_idmac_ring_to_cpu(host)` **before** the loop.

Therefore: **while every polled descriptor reads OWN-clear, the whole of pass 1
and pass 2 runs with the ring continuously CPU-owned and issues exactly zero
cache-maintenance operations after that single opening `ring_to_cpu()`.** Every
mask can be built from plain loads inside that window:

| mask | sample point | cost |
|---|---|---|
| `own_before_mask` | immediately after the existing `ring_to_cpu()` at the top of pass 1, before the poll loop | n plain loads |
| `own_after_poll_mask` | after the pass-1 loop, before pass 2 | n plain loads |
| `own_submit_mask`, `fd_mask`, `ld_mask`, `ch_mask` | in the existing `if (record) {...}` block after the FD/LD/CH fixups | n plain loads on lines the CPU just dirtied |
| `own_finish_mask` | in `dw_mci_cmd53_trace_finish()`, after `dw_mci_dmac_complete_dma()` has already called `ring_to_cpu()` | n plain loads |

`own_before_mask` needs `required` to know how many descriptors to sweep - which
the pre-count of §24.3 already computed, so the two requirements compose rather
than conflict. It is also a *stronger* sample than 0011's scalar `own_before`:
it is the state before any descriptor of this transfer has been polled or
written, rather than the state of descriptor 0 at its own poll.

**MUST NOT** build any mask by calling `dw_mci_idmac_own32()`/`own64()` or
`ring_to_cpu()`/`ring_to_device()`. In the settled case such a call happens to
be free - which is exactly the trap. Its cost would then be conditional on the
property under test, and in the unsettled case (a descriptor still OWN-set) each
extra call adds a `dma_sync_single_for_device` plus a later
`dma_sync_single_for_cpu` over the whole 4096-byte ring that 0013 would never
have issued. The rule is unconditional: plain loads only, at points where
existing code has already established CPU ownership.

**MUST NOT** add an `own_dma_irq_mask`. `dw_mci_cmd53_trace_own()` reads
`sg_cpu[0].des0` from the hard IRQ handler with no synchronisation at all, while
the ring is device-owned. That is the one sample point in 0011 with no ownership
guarantee, which is the reason `own_dma_irq` is excluded from the validity
criteria; widening an unsound read to n descriptors multiplies the unsoundness
without adding evidence.

**A defect in 0013's own assertion, found while establishing the above.** 0013
guards its geometry with

```c
WARN_ON_ONCE(!IS_ALIGNED(host->sg_dma, dma_get_cache_alignment()) || ...);
```

`dma_get_cache_alignment()` on this build almost certainly returns **1**:
`riscv_set_dma_cache_alignment()` runs in `setup_arch()` and clamps
`dma_cache_alignment` to 1 while `noncoherent_supported` is still false, and the
only thing that sets it on this SoC is `esp32p4_cache_init()` at
`arch_initcall`, which runs later. `IS_ALIGNED(x, 1)` is unconditionally true,
so **the assertion cannot fire**. The geometry it was meant to prove is still
correct - a page-aligned allocation is 64-byte aligned for free - but the check
proving it is dead code. This is §14.2 again, on 0013's own instrumentation: a
check that has never been observed failing, and in this case *cannot* fail. The
cheap fix is one `pr_info` of `dma_get_cache_alignment()` and
`L1_CACHE_BYTES` at probe, and asserting against the latter. It is a separate,
default-on-safe change; it is **not** bundled into 0014, because 0014's control
run has to be inert and adding a probe-time print to it would weaken that claim.
Recorded here so it is not lost.

### 24.5 The evidence can be truncated, and the byte count is a one-sided test

`cmd53_rx_trace_show()` accumulates `length` across three `sysfs_emit_at()`
calls per record. `sysfs_emit_at()` is `vscnprintf(buf + at, PAGE_SIZE - at,
...)` (`fs/sysfs/file.c:788`), and `vscnprintf()` truncates **mid-string**,
returning `size - 1` when it does (`lib/vsprintf.c:2984-2997`). So a record
whose lines straddle the page boundary is written **partially**: a header count
of four `CMD53HOST` lines does not exclude truncation part-way through record 4.

Two consequences, both worth stating precisely.

**The existing guard is dead.** `if (length >= PAGE_SIZE) break;` can never
fire, because `length` is bounded by `at + (PAGE_SIZE - at - 1) = PAGE_SIZE - 1`
= 4095. The loop runs to completion emitting nothing, and the read returns 4095.
No `WARN` fires either - `sysfs_emit_at()` only warns for `at >= PAGE_SIZE`,
which is unreachable for the same reason.

**The byte count is therefore a one-sided test, and must be stated as one.**
`vscnprintf()` returns the generated length `i` when `i < size`, and `size - 1`
only when it does not fit (`lib/vsprintf.c:2984-2997`). Against `size = 4096`
that means a read of exactly 4095 is produced *either* by truncation *or* by
content that happened to fit in exactly 4095 bytes. The two are
indistinguishable from outside:

```text
read length <  4095   => no PAGE_SIZE truncation occurred
read length == 4095   => truncated, or an exact 4095-byte fit; cannot tell
                      => fail closed: evidence invalid, not "truncated"
```

So `< 4095` is the acceptance condition, and `== 4095` **invalidates the run's
evidence** rather than proving a defect. Recording it the other way round would
be the §14.2 mistake in its most embarrassing form - a check asserting more than
it can see. No in-band marker can rescue the ambiguous case either, because a
marker emitted into a full buffer is the first thing to be lost. 4095 is derived
from the kernel's `PAGE_SIZE`, not read off the artifact under test.

Budget, for context rather than as the gate: measured Run 3 records are 557 B
each (145+128+281 plus newlines), worst-case field widths give ~692 B. Four
records are 2,228 B measured - which is exactly the size of the Run 3 capture,
so the existing evidence is provably complete. Adding one mask line of ~60 B
per record gives ~617 B measured / ~752 B worst case; four records stay under
3,100 B. The depth-8 ring is where it bites: eight records already exceed the
page **today**, before any mask line. That is a pre-existing limitation of 0011,
not something 0014 introduces, and it is why the four boot receives - and only
those - are the evidence.

Completeness is therefore gated by three conditions together, and an explicit
end marker is not needed:

```text
- exactly 4 records, seq 1..4
- seq 4 parses completely, through its final mask line
- attribute read length < PAGE_SIZE - 1 (= 4095)
```

Put the mask line **last** in each record and stamp it `seq=`, so "parses
completely" is a property of the last line of the last record rather than of a
header count. The first two conditions prove the right four records are present
and whole; the third excludes the case where a fifth record was cut in a way the
first two cannot see.

### 24.6 Pre-registered expectations

Fixed here, before any run, and not to be revised after seeing a result.

Descriptor counts, from the cap we set and the lengths §23 measured three times:

| seq | bytes | cap 4096 (control) | cap 16 | cap 8 | 64-byte line span at cap 8 |
|---:|---:|---:|---:|---:|---|
| 1 | 40 | 1 | 3 | 5 | line 0 holds 0-3, **desc 4 crosses into line 1** |
| 2 | 32 | 1 | 2 | 4 | exactly one line |
| 3 | 40 | 1 | 3 | 5 | **crosses** |
| 4 | 36 | 1 | 3 | 5 (8×4 + 4) | **crosses** |

Masks for a chain of `n` descriptors:

| mask | expected | at n=5 |
|---|---|---|
| `own_before_mask` | `0x00` | `0x00` |
| `own_after_poll_mask` | `0x00` | `0x00` |
| `own_submit_mask` | `(1<<n)-1` | `0x1f` |
| `own_finish_mask` | `0x00` | `0x00` |
| `fd_mask` | `0x01` | `0x01` |
| `ld_mask` | `1<<(n-1)` | `0x10` |
| `ch_mask` | `(1<<(n-1))-1` | `0x0f` |

`own_submit_mask` is `(1<<n)-1` and not `(1<<n)-1` minus the last bit because
the sample is taken **after** the FD/LD/CH fixups, which clear `CH`/`DIC` and
set `LD` on the last descriptor but leave `OWN` set on all of them. `ch_mask`
excludes the last descriptor for the same reason.

Acceptance, applied at the descriptor boundary:

1. `desc_count` exactly as tabulated, and `slots` `0-(n-1)`. `desc_count >= 2`
   is a **gate, not an observation**: counts stuck at 1 mean the parameter did
   not take effect and the run produced no evidence about the multi-descriptor
   path, whatever else it shows. `len=` - which 0011 samples as BS1 of the
   *first* descriptor (`0011:263`, `:320`) - must equal `min(cap, bytes)`, i.e.
   8 on every record at cap 8 and 16 at cap 16, against 40/32/40/36 on the
   control. It is the one field that shows the cap reached the descriptor
   writer rather than only the counter.
2. All seven masks equal to the table.
3. `chunk=` in the trace equals the value written into `bootargs`, and equals
   the value read from `/sys/module/dw_mmc/parameters/`. Three independent
   statements required to agree (§14.16).
4. Everything §23.2 required still holds: `start=0`, `engine=idmac`,
   `own_timeout=0`, `pio=0`, `err=0`, `idsts=0000a102` with `dto=1 dma=1
   dataerr=0`, and `bytes_xfered` still 40/32/40/36 - chunking must not change
   the totals.
5. The captured `cmd53_rx_trace` holds exactly four records, seq 1-4; seq 4
   parses completely through its final mask line; and the attribute read is
   **shorter than 4095 bytes**. A read of exactly 4095 makes the run's evidence
   invalid rather than failed (§24.5).
6. Strict-cold protocol identical to Runs 1-3, including the >= 5.000 s absence
   and the armed-before-disconnect ordering.

`own_dma_irq` is **not** a validity criterion, for the reason given in §24.4.

A capacity reject (`start=-28`, `count=0`, `dev_warn_once` in the log, `pio=N`)
on a *later, larger* transfer is **not a failure** at cap 8. Exactly 2048 bytes
consumes all 256 slots and is accepted; anything above 2048 bytes needs a 257th
descriptor and is rejected by construction (§24.3). It is a failure only if it
appears on seq 1-4. If more than eight CMD53 reads occur before the shell
is reached, the depth-8 ring overwrites seq 1-4 and the run yields no evidence -
Runs 1-3 produced exactly four, so this is a known-quantity risk, not a
prediction.

### 24.7 Static confirmation that the small caps are legal

* **Representable.** `IDMAC_SET_BUFFER1_SIZE` masks with `0x1fff`
  (`dw_mmc.c:96`): BS1 is a 13-bit field, so 8 is representable and the field
  maximum is 8191, above the current 4096.
* **Bus alignment.** Every resulting buffer length is a multiple of 4 -
  8,8,8,8,8 / 8,8,8,8 / 8,8,8,8,8 / 8,8,8,8,4 - and every buffer address is
  8-byte aligned, because the measured base VAs are `0x48c74940` and
  `0x48c75000` and the offsets are multiples of 8.
* **Not pushed to PIO.** `DW_MCI_DMA_THRESHOLD` is 16, applied to
  `data->blocks * data->blksz` (`dw_mmc.c:890`) - the whole transfer, not the
  per-descriptor chunk. 40, 32 and 36 all stay above it.
* **Not statically confirmed:** whether the controller tolerates a descriptor
  buffer shorter than the FIFO burst it would otherwise issue. `SDMMC_FIFOTH`'s
  msize is computed from `blksz` at runtime (`dw_mmc.c:994-1026`), and the
  DesignWare databook is not in this repository. The same gap covers the 4-byte
  tail descriptor a 36-byte transfer produces at cap 8. This is the residual
  risk of the 8-byte value, and it is why 16 is tested first.

### 24.8 Procedure

Three runs, each stopping for classification before the next. Only one variable
moves per step.

1. **Build** `0013 + 0014`, parameter defaulted off, one `Image`. Before
   flashing, confirm by reading the emitted `dw_mmc.c` that the disabled path
   computes exactly `DW_MCI_DESC_DATA_LENGTH`, that nothing outside the CMD53 RX
   gate sees a changed cap, that the pre-count precedes the first poll, and that
   no mask sample sits on a path that issues a DMA sync.
2. **Control run**: unchanged `bootargs`, one strict-cold boot. Requires the
   complete §23 baseline including `desc_count=1/1/1/1`. This is what shows 0014
   is inert when off; without it, any later difference is confounded with "we
   changed the kernel".
3. **Cap 16**: rewrite only the 64 KiB DTB region, one strict-cold boot.
   Requires `3/2/3/3` and the §24.6 criteria.
4. **Cap 8**: rewrite only the DTB again, one strict-cold boot. Requires
   `5/4/5/5`, with the 40-byte transfers spanning descriptors 0-4 and so
   crossing from the first 64-byte descriptor cache line into the second.

How to read each outcome - fixed in advance so the staging is legible rather
than reconstructed afterwards:

| outcome | reading |
|---|---|
| Control fails | 0014 is not inert when off. Do not proceed; the fault is in the patch, not in the ownership model. |
| Control passes, 16 fails | A multi-descriptor problem **within one cache line**. Ownership handover across descriptors is implicated; line sharing is not. |
| 16 passes, 8 fails | Either the line **crossing** or the 8-byte / 4-byte non-final descriptor. These two are not separated by this experiment; §24.7's unconfirmed FIFO-burst question is the first suspect. |
| Both pass | Strong evidence that 0013's ownership discipline holds across descriptors and across cache lines, for this traffic. |

After 16 and 8 both pass, the next decision is **not** why2025 0015. It is
whether TX multi-descriptor needs its own gate: 0013's ownership model is
RX/TX common, this experiment exercises only RX, and the 512-byte boot TX at a
cap of 8 would be 64 descriptors across 16 cache lines - a materially harder
case that the RX-only trace cannot classify. That decision is taken on its own
evidence, not folded in here.

### 24.9 What this experiment will still not show

* The chunking is synthetic. Production multi-descriptor traffic comes from
  transfers above 4096 bytes or from multi-segment scatter lists, and this gate
  produces neither. It does reproduce the cache-line sharing and crossing the
  ownership discipline exists for, which is the property under test - but that
  is not a claim about production traffic patterns.
* TX is untouched by design, so nothing here classifies the TX chain (§24.8).
* Whether the IDMAC prefetches past `LD`, and whether it tolerates an 8-byte or
  4-byte descriptor buffer. Both need the databook or a measurement.
* One board, one silicon revision, one C6 firmware, cold boot only.
* Nothing about Wi-Fi association, DHCP, SSH, sustained traffic, or the
  pre-existing SDIO function-2 `-22`.
* The gate must be absent, or provably default-off, in any image proposed for
  `main`, and the `ttyGS1` diagnostic shell must not survive into acceptance.

### 24.10 Corrections to the superseded §24, and what remains unverified

Three defects in the draft committed as `7ca62ed`:

1. **Cap scope.** It applied the cap to all IDMAC transfers, introducing an
   unobserved second variable (512 B TX → 64 descriptors) on an RX-only trace.
   Narrowed to CMD53 reads (§24.2).
2. **`ring_size - 1` bound.** It reserved the last slot to protect
   `IDMAC_DES0_ER`. The reservation does not protect it - the fill loop assigns
   `des0` - and upstream makes that slot reachable by design. Corrected to
   `required <= host->ring_size` (§24.3).
3. **Factual error about the SDIO path.** It stated that `mmc_io_rw_extended()`
   ignores `max_seg_size`. It does not; `max_segs` is what the SDIO path
   ignores. The conclusion (an explicit pre-count is required) survives, but the
   reason was wrong (§24.3).

Claims here that a single reader closed, and that independent refutation would
have been worth having - the three refuters and the completeness critic died on
a session limit:

* That no mask sample point can issue a cache operation. The argument rests on
  `ring_to_cpu()`/`ring_to_device()` being early-return no-ops in the current
  domain and on pass 1 opening with a `ring_to_cpu()`; both are quoted from
  0013, but nothing has executed to confirm the compiler does not reorder a
  sample across a helper call.
* That `record != NULL` is exactly equivalent to the open-coded CMD53-read
  predicate. Traced through `trace_begin()` and `__dw_mci_start_request()`, not
  tested.
* That `-ENOSPC` is distinguishable end to end. `start_ret` is `s32` and printed
  with `%d`, so `-28` will render - but no run has produced one.
* That `dma_get_cache_alignment()` returns 1 on this build. Inferred from the
  `setup_arch` → `arch_initcall` ordering, not observed; the cheap confirmation
  is a `pr_info`.

Nothing in this section may be built or flashed until it is reviewed.

## 25. Control run: 0014 present, parameter default 0

§24.8 step 2. One `Image` carrying 0013+0014, runtime parameter left at 0,
DTB byte-identical to the no-diagnostic-bootarg file
`0fb1f66abdc4dfd19d14c7543cb6acddeb99ee72f430f42cfce9218e7c19528d`. No
why2025 0015, no DTB rewrite, no C6 write, no association, DHCP, SSH or
network stress.

The build-script provenance gap from the 0014 patch commit is closed:
`e5b48705138fb6fcdd322caf55b9c930cbfa7b24` wires
`EASYSTICK_CMD53_RX_DESC_BYTES` and was the same `build-m1.sh` blob
(`fc2e421a9955f940b25ad3290e9486a0268b6371`) used for the completed clean
build. The 0014 patch remains
`6ccb218f5ede5cc10b934bc4bfefab3285da25a39bae5797027d453f325cc019`.

### 25.1 Flash

Pre-write `verify_flash` on COM10 (esptool 4.8.1, `--after no_reset`) matched
the 0013 Image `c8b2cf59d339d084bd4b0d019d286c2c0f93aa3fb768c5240df7de6ed8b61d8e`
and the five frozen companion regions. `chip_id` reported ESP32-P4 v1.3, MAC
`e8:f6:0a:e2:5e:73`. Only `0x90000` was written:

`62d1b49bd9d5671a2fb36792fe9645579792e8860d5fa736c530697e5a98372b`
(6,576,968 B). Independent `verify_flash` of that Image matched. A following
`verify_flash` of the DTB still matched `0fb1f66a…19528d`. `erase_flash` was
not used. Bootloader, partition table, boot-shim, rootfs, DTB, NVS and PHY
were not written.

### 25.2 Strict-cold conditions

The watcher was armed **before** the disconnect and used only
`SerialPort::GetPortNames()` while waiting.

| Property | Value |
|---|---|
| Armed (Asia/Tokyo) | 2026-08-15T12:34:58.463 |
| COM10 present at arm | `GetPortNames()` immediately before, and again at arm |
| Detached (UTC) | 2026-08-15T03:38:15.660 |
| Returned (UTC) | 2026-08-15T03:38:24.127 |
| Absence (gate: >= 5.000 s) | **8.467 s**, accepted |
| `control_lines_driven` / `reset_requested` | false / false |
| Passive capture | 90 s, 14,673 B |
| `port_opens` | 21 |
| First byte | 2.017 s |
| C6 last reset cause | `POWERON_RESET` |
| Kernel stamp | `#1 Sat Aug 15 02:29:19 UTC 2026` |

`port_opens=21` and a 2.017 s first byte are a capture-quality deviation from
§23's `port_opens=1` / ~0.03 s. The first captured line is already mid-banner
(`ld (GNU Binutils)…`), so the ROM/`rst:` word is not in this file. The
acceptance criterion that *is* in the log is the C6 `POWERON_RESET`. The
later read-only shell opened COM10 with DTR/RTS false, listened 6 s, and
saw **no boot banner**; the prompt was already at `~ #`. The sysfs read
below therefore belongs to this cold boot, not to a later one.

### 25.3 Parameter and cmdline

`/sys/module/dw_mmc/parameters/easystick_cmd53_rx_desc_bytes` = **0**.
`/proc/cmdline` is the frozen no-diagnostic line
(`earlycon=… idle=poll`) and does not name the parameter.

### 25.4 Descriptor-boundary classification

`wc -c` on `/sys/bus/platform/devices/50083000.mmc/cmd53_rx_trace` = **2460**
(< 4095). Four complete records, seq 1-4, each with a mask line:

| seq | bytes | chunk | len | count | slots | start | engine | own_timeout | idsts / dto dma dataerr | err | pio | own scalars | masks own/fd/ld/ch |
|---:|---:|---:|---:|---:|---|---:|---|---:|---|---:|---:|---|---|
| 1 | 40 | 4096 | 40 | 1 | 0-0 | 0 | idmac | 0 | `0000a102` / 1 1 0 | 0 | 0 | `00000000/00000000/8000000c/8000000c/0000000c` | `00/00/01/00` `01` `01` `00` |
| 2 | 32 | 4096 | 32 | 1 | 0-0 | 0 | idmac | 0 | `0000a102` / 1 1 0 | 0 | 0 | `0000000c/0000000c/8000000c/8000000c/0000000c` | `00/00/01/00` `01` `01` `00` |
| 3 | 40 | 4096 | 40 | 1 | 0-0 | 0 | idmac | 0 | `0000a102` / 1 1 0 | 0 | 0 | `0000000c/0000000c/8000000c/8000000c/0000000c` | `00/00/01/00` `01` `01` `00` |
| 4 | 36 | 4096 | 36 | 1 | 0-0 | 0 | idmac | 0 | `0000a102` / 1 1 0 | 0 | 0 | `0000000c/0000000c/8000000c/8000000c/0000000c` | `00/00/01/00` `01` `01` `00` |

Mask fields are `own_before/own_after_poll/own_submit/own_finish` then
`fd`/`ld`/`ch`. All four are `0/0/1/0`, `fd=1`, `ld=1`, `ch=0`. The OWN
scalars are byte-identical to the §23.3 3-of-3 signature. `chunk=4096` is
the compile-time `DW_MCI_DESC_DATA_LENGTH` path, which is what parameter 0
must compute.

Secondary only, and in order: command `0x1` (40-byte boot event), 38-byte
payload RX (trace seq 3 at 40 bytes), command `0xF`, `wlan0`
`<BROADCAST,MULTICAST,UP,LOWER_UP>` MAC `10:bd:a3:9e:22:f4`. The
pre-existing `esp32_sdio mmc0:0001:2: probe … -22` is present and is not a
control-run criterion (§24.9).

**Control: PASS.** 0014 is inert at parameter 0. Cap=16 is not started here.

### 25.5 Evidence files

| File | Bytes | SHA-256 |
|---|---:|---|
| `easystick-p4-0014-control-prewrite-20260815.log` | 1744 | `1b458608eff638e75ea072a1e8f9af034417314c46555dab4f4a57b34d851407` |
| `easystick-p4-0014-control-write-20260815.log` | 7219 | `23a700dc9847dd03d1a49c3cabef3a144f9bf3982a5aaae5a45a815784b4471b` |
| `easystick-p4-0014-control-verify-20260815.log` | 1245 | `e3dd28d1bbbf2b53d4f739332dae182d6c088dfbe406e6e719d467aca17984f4` |
| `easystick-p4-0014-control-strictcold-detector-20260815.txt` | 445 | `929b8ca4c03d68ac9d3a3a57fa982153e3c1e9153be7feb57bd3190ad335ba42` |
| `easystick-p4-0014-control-strictcold-20260815.log` | 14673 | `cdfdbcf303c31ca1a1edb14eb31529ac4a87357a9daef721f54cd777f8f695c7` |
| `easystick-p4-0014-control-strictcold-20260815.log.json` | 431 | `ed6d722f332725f214427ee96e733ab32f3c3b4e469207b388069c466c07bf6f` |
| `easystick-p4-0014-control-strictcold-shell-20260815.log` | 6729 | `34abd846407f768cb5a345c172321beaef7820a485d359bc72e275895588f5fa` |

All under `C:\Users\developer\tmp\`.

## 26. Cap=16 DTB rewrite: parameter did not reach the module

§24.8 step 3. Same `Image` as §25
(`62d1b49bd9d5671a2fb36792fe9645579792e8860d5fa736c530697e5a98372b`).
Only the 64 KiB DTB region at `0xf10000` was rewritten. No C6 write, no
Image rewrite, no why2025 0015.

### 26.1 DTB construction and flash

The control DTB `0fb1f66a…19528d` was decompiled and recompiled with `dtc`
from the 0014 host tools. A no-edit round-trip was byte-identical. The only
source change was appending
`dw_mmc.easystick_cmd53_rx_desc_bytes=16` to `/chosen/bootargs`.

| Artifact | Bytes | SHA-256 |
|---|---:|---|
| `cap16.dtb` | 3124 | `36a7389a9f9bead1c4c9f10ca7f84e174a806e46c4848d966c46fe3e29b252b5` |
| `cap16.decompiled.dts` | 3368 | `74759e85474d05f9d97dcd43308de42ae627cdb3c2ff869fe2e326964eac2fdf` |

Pre-write `verify_flash` on COM10 (esptool 4.8.1, `--after no_reset`) matched
the §25 Image and the five frozen companion regions, including the then-current
DTB `0fb1f66a…19528d`. `chip_id` reported ESP32-P4 v1.3, MAC
`e8:f6:0a:e2:5e:73`. Only `0xf10000` was written. Independent `verify_flash`
of that DTB matched `36a7389a…b252b5`. A following `verify_flash` of the
Image still matched `62d1b49b…8372b`. `erase_flash` was not used.

### 26.2 Strict-cold conditions

The watcher was armed **before** the disconnect and used only
`SerialPort::GetPortNames()` while waiting.

| Property | Value |
|---|---|
| Armed (Asia/Tokyo) | 2026-08-15T13:00:59.050 |
| COM10 present at arm | true |
| Detached (UTC) | 2026-08-15T04:17:51.605 |
| Returned (UTC) | 2026-08-15T04:18:00.039 |
| Absence (gate: >= 5.000 s) | **8.434 s**, accepted |
| `control_lines_driven` / `reset_requested` | false / false |
| Passive capture | 90 s, 17,574 B |
| `port_opens` | 20 |
| First byte | 2.011 s |
| C6 last reset cause | `POWERON_RESET` |
| Kernel stamp | `#1 Sat Aug 15 02:29:19 UTC 2026` |

`port_opens=20` and a 2.011 s first byte are the same capture-quality
deviation as §25.2. The later read-only shell opened COM10 with DTR/RTS
false, listened 6 s, and saw **no boot banner**.

### 26.3 Three statements of the parameter disagreed

`/proc/device-tree/chosen/bootargs` **does** contain
`dw_mmc.easystick_cmd53_rx_desc_bytes=16`. That is the flashed DTB, and it
is what the boot-shim passed in `a1`.

`/proc/cmdline` is the frozen no-diagnostic line and does **not** name the
parameter. `/sys/module/dw_mmc/parameters/easystick_cmd53_rx_desc_bytes` =
**0**. Probe did not print
`easystick_cmd53_rx_desc_bytes=16: CMD53 RX descriptor chunk diagnostic active`.

The 0014 Image was built with `CONFIG_CMDLINE_FORCE=y` and
`CONFIG_CMDLINE` equal to that frozen line. The kernel therefore ignores
`/chosen/bootargs` for module parameters. §24.8 step 3's "rewrite only the
DTB" cannot arm 0014 on this Image. The DTB write is not a failed flash; it
is a failed assumption about which object owns `bootargs`.

### 26.4 Descriptor-boundary classification

`wc -c` on `cmd53_rx_trace` = **2460** (< 4095). Four complete records,
seq 1-4. Every field that §24.6 uses as a cap=16 gate is the §25 control
value, not the cap=16 value:

| seq | bytes | chunk | len | count | slots | start | engine | own_timeout | idsts / dto dma dataerr | err | pio | masks own/fd/ld/ch |
|---:|---:|---:|---:|---:|---|---:|---|---:|---|---:|---:|---|
| 1 | 40 | 4096 | 40 | 1 | 0-0 | 0 | idmac | 0 | `0000a102` / 1 1 0 | 0 | 0 | `00/00/01/00` `01` `01` `00` |
| 2 | 32 | 4096 | 32 | 1 | 0-0 | 0 | idmac | 0 | `0000a102` / 1 1 0 | 0 | 0 | `00/00/01/00` `01` `01` `00` |
| 3 | 40 | 4096 | 40 | 1 | 0-0 | 0 | idmac | 0 | `0000a102` / 1 1 0 | 0 | 0 | `00/00/01/00` `01` `01` `00` |
| 4 | 36 | 4096 | 36 | 1 | 0-0 | 0 | idmac | 0 | `0000a102` / 1 1 0 | 0 | 0 | `00/00/01/00` `01` `01` `00` |

OWN scalars match the §23.3 3-of-3 signature. `chunk=4096` is the
compile-time `DW_MCI_DESC_DATA_LENGTH` path.

**Cap=16: no evidence.** §24.6 item 1: `desc_count` stuck at 1 means the
parameter did not take effect. This run does not speak to the
multi-descriptor path. Cap=8 is not started.

### 26.5 SSH attempt on the same boot

Tried after classification, on this same cold boot, without flashing.

`wlan0` came up `10:bd:a3:9e:22:f4` as in §25. `S00mounts` does not mount
the fstab tmpfs entries, so `/tmp` is the squashfs directory and is
read-only. After a manual `mount -t tmpfs tmpfs /tmp` (and `/run`),
`wpa_supplicant -B` started (pid 386, exit 0). Scan traffic appeared
(CMD53 RX lengths 458/375/286). `iw dev wlan0 link` stayed `Not connected`
for 45 s. `wpa_cli` is not on the image. No IPv4. SSH was not attempted.

`S85easystick-ssh` is a second, independent blocker: `/usr/sbin/inetd` is a
BusyBox symlink whose applet is not compiled in (`inetd: applet not found`).
The script's `-x` test therefore passes and then the start fails. Even a
later association would need a standalone `dropbear` invocation, not this
inetd path.

### 26.6 What this does and does not authorize

It authorizes one conclusion: **arming 0014 on this Image requires changing
`CONFIG_CMDLINE` (or clearing `CONFIG_CMDLINE_FORCE`) and rewriting the
Image region**. A DTB-only rewrite cannot do it.

It does **not** authorize rebuilding, flashing a new Image, writing C6,
adding why2025 0015, or treating the §25 control as invalidated. The
control remains the inert-at-zero proof. The next cap=16 attempt is a new
Image variable and needs its own review before flash.

### 26.7 Evidence files

| File | Bytes | SHA-256 |
|---|---:|---|
| `easystick-p4-0014-cap16-prewrite-20260815.log` | 1747 | `186b6c4b93b13c9d01f9adbedb7cfc76f01c7affe6b99767acf5361faf4e3d26` |
| `easystick-p4-0014-cap16-write-20260815.log` | 559 | `6f4a03f68778897ddd7818ccda9b70de88530f24edf0b76a23fc95cdf87c464a` |
| `easystick-p4-0014-cap16-verify-20260815.log` | 1224 | `6edaceb4d388500cbdafe30f3671911a28606f02d337c76960600f54e68f80c4` |
| `easystick-p4-0014-cap16-dtb-20260815/cap16.dtb` | 3124 | `36a7389a9f9bead1c4c9f10ca7f84e174a806e46c4848d966c46fe3e29b252b5` |
| `easystick-p4-0014-cap16-dtb-20260815/cap16.decompiled.dts` | 3368 | `74759e85474d05f9d97dcd43308de42ae627cdb3c2ff869fe2e326964eac2fdf` |
| `easystick-p4-0014-cap16-strictcold-detector-20260815.txt` | 468 | `81700d59bf4c99050874f666bdc91a437213357efa15edf8a8ac4ff1e58b20e6` |
| `easystick-p4-0014-cap16-strictcold-20260815.log` | 17574 | `b6f6b897d36979fb8ea6e46632c23fc98d665c2583788ddc3940af9fa62f6926` |
| `easystick-p4-0014-cap16-strictcold-20260815.log.json` | 431 | `46f0142f5c287a204cf81f8d8bbe41467c50e1ae5f706ad1d2eb02341c0e4039` |
| `easystick-p4-0014-cap16-strictcold-shell-20260815.log` | 7594 | `4423a5fb099969361bf443e10fa7fe462794fba340e55bee3fc3665396541229` |
| `easystick-p4-0014-cap16-ssh-20260815.log` | 770 | `a8eacd1c78220d53fd03ebd4a41c08a5360efd23e3b5d38b41b70a3fd4775d96` |
| `easystick-p4-0014-cap16-net-ssh-shell-20260815.log` | 4947 | `e578cc084fc4b14b58be8710177db86ba10e83c849a1c6c101d5314dbfe82416` |
| `easystick-p4-0014-cap16-net-ssh2-shell-20260815.log` | 34545 | `9d3e94adcf16b57f688fd80262fb9772716c244e8454d4736bdad543f1f3cf4c` |
| `easystick-p4-0014-cap16-wpa-status-20260815.log` | 744 | `ee237aef6dd494ade38ddf944019ccb2df6b85d753e2ce9d79b3b4864096e49b` |

All under `C:\Users\developer\tmp\` except the two DTB artifacts in
`C:\Users\developer\tmp\easystick-p4-0014-cap16-dtb-20260815\`.

## 27. Cap=16 after honoring DTB bootargs

§24.8 step 3, retry. The DTB is still
`36a7389a9f9bead1c4c9f10ca7f84e174a806e46c4848d966c46fe3e29b252b5`.
The Image is new: `CONFIG_CMDLINE_FORCE` cleared in `linux.config` and
`m2/kernel.config.fragment`, `CONFIG_CMDLINE_FALLBACK=y`. That is the
variable §26.6 named. 0013+0014 remain. No C6 write, no why2025 0015.

This Image is therefore not the §25 control Image. The control still
proves 0014 is inert at parameter 0 on the FORCE=y build. This run asks
whether cap 16, once it actually reaches `dw_mmc`, produces the
pre-registered multi-descriptor geometry.

### 27.1 Flash

Incremental rebuild on the §25 output volume. Emitted
`linux-custom/.config` has `CONFIG_CMDLINE_FALLBACK=y` and
`# CONFIG_CMDLINE_FORCE is not set`. `dw_mmc.c` still contains the 0014
parameter and the probe banner. New Image:

`75fe932477a967d8f02b5fe1812a724010acd8e377a3d98b89b952fe8def88d0`
(6,576,968 B). Kernel stamp `#2 Sat Aug 15 04:58:45 UTC 2026`.

Pre-write `verify_flash` matched the §25 Image `62d1b49b…8372b`, the
cap=16 DTB, and the four other frozen regions. `chip_id` reported
ESP32-P4 v1.3, MAC `e8:f6:0a:e2:5e:73`. Only `0x90000` was written.
Independent `verify_flash` of that Image matched. A following
`verify_flash` of the DTB still matched `36a7389a…b252b5`.

### 27.2 Strict-cold conditions

| Property | Value |
|---|---|
| Armed (Asia/Tokyo) | 2026-08-15T14:01:49.847 |
| COM10 present at arm | true |
| Detached (UTC) | 2026-08-15T05:03:32.956 |
| Returned (UTC) | 2026-08-15T05:03:54.401 |
| Absence (gate: >= 5.000 s) | **21.445 s**, accepted |
| `control_lines_driven` / `reset_requested` | false / false |
| Passive capture | 90 s, 18,510 B |
| `port_opens` | **1** |
| First byte | **0.218 s** |
| C6 last reset cause | `POWERON_RESET` |
| Kernel stamp | `#2 Sat Aug 15 04:58:45 UTC 2026` |

`port_opens=1` and a 0.218 s first byte meet the §23 capture-quality
shape that §25 and §26 missed. The later shell listened 6 s with DTR/RTS
false and saw no boot banner.

### 27.3 Three statements of the parameter now agree

`/proc/device-tree/chosen/bootargs`, `/proc/cmdline`, and
`/sys/module/dw_mmc/parameters/easystick_cmd53_rx_desc_bytes` all carry
**16**. Probe printed
`easystick_cmd53_rx_desc_bytes=16: CMD53 RX descriptor chunk diagnostic active`.
`chunk=` in every trace record is 16.

### 27.4 Descriptor-boundary classification

`wc -c` on `cmd53_rx_trace` = **2452** (< 4095). Four complete records,
seq 1-4, each with a mask line:

| seq | bytes | chunk | len | count | slots | start | engine | own_timeout | idsts / dto dma dataerr | err | pio | own scalars | masks own/fd/ld/ch |
|---:|---:|---:|---:|---:|---|---:|---|---:|---|---:|---:|---|---|
| 1 | 40 | 16 | 16 | 3 | 0-2 | 0 | idmac | 0 | `0000a102` / 1 1 0 | 0 | 0 | `00000000/00000000/8000001a/8000001a/0000001a` | `00/00/07/00` `01` `04` `03` |
| 2 | 32 | 16 | 16 | 2 | 0-1 | 0 | idmac | 0 | `0000a102` / 1 1 0 | 0 | 0 | `0000000c/0000000c/8000001a/8000001a/0000001a` | `00/00/03/00` `01` `02` `01` |
| 3 | 40 | 16 | 16 | 3 | 0-2 | 0 | idmac | 0 | `0000a102` / 1 1 0 | 0 | 0 | `0000000c/0000000c/8000001a/8000001a/0000001a` | `00/00/07/00` `01` `04` `03` |
| 4 | 36 | 16 | 16 | 3 | 0-2 | 0 | idmac | 0 | `0000a102` / 1 1 0 | 0 | 0 | `0000000c/0000000c/8000001a/8000001a/0000001a` | `00/00/07/00` `01` `04` `03` |

`desc_count` is **3/2/3/3**. `len=` is **16** on every record
(`min(16, bytes)`). Masks match §24.6 at n=3 (`own_submit=0x07`,
`ld=0x04`, `ch=0x03`) and n=2 (`0x03` / `0x02` / `0x01`). Totals remain
40/32/40/36. `start=0`, `engine=idmac`, `own_timeout=0`, `pio=0`,
`err=0`, `idsts=0000a102`. The OWN scalars are not the §23 n=1
`8000000c` signature; `8000001a` is the multi-descriptor DES0 value
after FD/LD/CH fixups and is consistent across all four records.

Secondary only, and in order: command `0x1` (40-byte boot event),
38-byte payload RX (trace seq 3), command `0xF`, `wlan0`
`<BROADCAST,MULTICAST,UP,LOWER_UP>` MAC `10:bd:a3:9e:22:f4`. The
pre-existing `esp32_sdio mmc0:0001:2: probe … -22` is present.

**Cap=16: PASS.** 0013's ownership discipline held across two and three
descriptors inside one 64-byte cache line, for this traffic. Cap=8 is
not started here.

### 27.5 SSH attempt on the same boot

Tried after classification. `wlan0` was UP. tmpfs on `/tmp` and `/run`
succeeded. `wpa_supplicant -B` returned 0. `iw dev wlan0 link` stayed
`Not connected` for 30 s. No IPv4. Standalone `dropbear` returned 1.
SSH was not attempted. This does not reopen the cap=16 gate.

### 27.6 Evidence files

| File | Bytes | SHA-256 |
|---|---:|---|
| `easystick-p4-0014-cmdline-fallback-candidate-20260815/Image` | 6576968 | `75fe932477a967d8f02b5fe1812a724010acd8e377a3d98b89b952fe8def88d0` |
| `easystick-p4-0014-fallback-prewrite-20260815.log` | 1726 | `0ad59a8919c1d12935f81ef5ff321393f9a4b458e767b84aaf4553586ef75471` |
| `easystick-p4-0014-fallback-write-20260815.log` | 7219 | `0371d670300faeef72edbdd3bc9ed8268571c6d825321cea961fe695e64d2619` |
| `easystick-p4-0014-fallback-verify-20260815.log` | 1233 | `e1fa5ac8d7d51b260e72f98150be99b8fd525ff140a06a7e207c9c91aca8d7bc` |
| `easystick-p4-0014-fallback-strictcold-detector-20260815.txt` | 494 | `7857aceec1c7949a9a5d043e86028f29845fb73b885a87187bc6cef7f5bfe66d` |
| `easystick-p4-0014-fallback-strictcold-20260815.log` | 18510 | `2e54789e731fb72dabdf26285830aade6d7441baf0491eaf751c746acf835728` |
| `easystick-p4-0014-fallback-strictcold-20260815.log.json` | 226 | `bb04fd6cdbeb40b522409a3e4478a04cb92f9455e053f99ed7afb9ee34a51694` |
| `easystick-p4-0014-fallback-strictcold-shell-20260815.log` | 48010 | `d517e19b8392079cda0f560b4eb423636b21f524070953c98d2cd6522f9da903` |
| `easystick-p4-0014-fallback-ssh-20260815.log` | 434 | `e1280f90d6c496bf10a1bd59de8ff332185643d32ebda689d741779c5cf8f1e4` |

All under `C:\Users\developer\tmp\`.

## 28. Cap=8 after honoring DTB bootargs

§24.8 step for cap=8. Same fallback 0014 Image as §27
(`75fe932477a967d8f02b5fe1812a724010acd8e377a3d98b89b952fe8def88d0`).
DTB-only rewrite at `0xf10000` to
`easystick-p4-0014-cap8-dtb-20260815/cap8.dtb`
(`f3901be2d2fbfa05bd4affde2986bb47d396a21ddfb59d8c52c175ac69260ce7`,
3124 B). Independent `verify_flash` of DTB and Image both matched. C6
untouched.

### 28.1 Strict-cold conditions

| Property | Value |
|---|---|
| Armed (Asia/Tokyo) | 2026-08-15T15:12:26.838 |
| COM10 present at arm | true |
| Absence (gate: >= 5.000 s) | **9.584 s**, accepted |
| Kernel stamp | `#2 Sat Aug 15 04:58:45 UTC 2026` |
| C6 last reset cause | `POWERON_RESET` |

### 28.2 Three statements of the parameter agree

`/proc/cmdline`, the probe line, and
`/sys/module/dw_mmc/parameters/easystick_cmd53_rx_desc_bytes` all carry
**8**. `chunk=` in every seq-1–4 record is 8.

### 28.3 Descriptor-boundary classification

`wc -c` on `cmd53_rx_trace` = **2444** (< 4095). Four complete records,
seq 1-4, each with a mask line:

| seq | bytes | chunk | len | count | slots | start | engine | own_timeout | idsts / dto dma dataerr | err | pio | masks own/fd/ld/ch |
|---:|---:|---:|---:|---:|---|---:|---|---:|---|---:|---:|---|
| 1 | 40 | 8 | 8 | 5 | 0-4 | 0 | idmac | 0 | `0000a102` / 1 1 0 | 0 | 0 | `00/00/1f/00` `01` `10` `0f` |
| 2 | 32 | 8 | 8 | 4 | 0-3 | 0 | idmac | 0 | `0000a102` / 1 1 0 | 0 | 0 | `00/00/0f/00` `01` `08` `07` |
| 3 | 40 | 8 | 8 | 5 | 0-4 | 0 | idmac | 0 | `0000a102` / 1 1 0 | 0 | 0 | `00/00/1f/00` `01` `10` `0f` |
| 4 | 36 | 8 | 8 | 5 | 0-4 | 0 | idmac | 0 | `0000a102` / 1 1 0 | 0 | 0 | `00/00/1f/00` `01` `10` `0f` |

`desc_count` is **5/4/5/5**. `len=` is **8** on every record
(`min(8, bytes)`). Masks match §24.6 at n=5 (`own_submit=0x1f`,
`ld=0x10`, `ch=0x0f`) and n=4 (`0x0f` / `0x08` / `0x07`). Totals remain
40/32/40/36. Seq 1 and seq 3/4 cross a 64-byte line as predicted.
`start=0`, `engine=idmac`, `own_timeout=0`, `pio=0`, `err=0`,
`idsts=0000a102`.

**Cap=8: PASS.** 0013's ownership discipline held across four and five
descriptors, including the line-crossing case.

### 28.4 Association and L3 on the same boot

Association to BSSID `78:8c:b5:cb:02:43` reached
`process_assoc_event: Connection status: 0` and EAPOL `88 8e` while
still in the 4-way. Setting `10.255.10.199` at that moment produced
`Disconnect event … [reason:2]`. A later retry on the same boot, after
C6 was already dirty, timed out `Command[0xF]` and `Command[0xD]`. Host
ping and SSH to `.199` failed. This does not reopen the cap=8 gate.

The flashed squashfs still cannot host Dropbear without inetd. L3 must
work before that rootfs limit is the blocker.

### 28.5 Evidence files

| File | Bytes | SHA-256 |
|---|---:|---|
| `easystick-p4-0014-cap8-dtb-20260815/cap8.dtb` | 3124 | `f3901be2d2fbfa05bd4affde2986bb47d396a21ddfb59d8c52c175ac69260ce7` |
| `easystick-p4-wifi-cap8-write-20260815.log` | 559 | `f0d55ec2bcc5f5e43850c214dee195be9cfdc82547fad912d2777b0fe8af3098` |
| `easystick-p4-wifi-cap8-verify-20260815.log` | 788 | `14bb1b3edb9202d1c8c182d5915b99e07edffc29e597e2341073f8d47131d45e` |
| `easystick-p4-wifi-cap8-strictcold-detector-20260815.txt` | 121 | `bde91795d85416c320fe75834e3b1b5b336cbf2c81ba50df2e0f71f112af47eb` |
| `easystick-p4-wifi-ssh-cap8-20260815.log` | 44131 | `7fc049a2982e9afb0676d28a17000a8d18e99de5af2ecd7ec5f732455ad6726f` |
| `easystick-p4-wifi-ssh-cap8-4way-20260815.log` | 19290 | `56c9beb887e427fbb67d777211cbbc8519a1c0192bfd7566fade13973bad70f3` |

All under `C:\Users\developer\tmp\`.

## 29. Hosted 4-way restored; DUT-to-AP ping succeeded

After cap=8, association reached `Connection status: 0` and then
lost EAPOL message 3. C6 was not rewritten. Image
`75fe932477a967d8f02b5fe1812a724010acd8e377a3d98b89b952fe8def88d0`
and DTB `f3901be2d2fbfa05bd4affde2986bb47d396a21ddfb59d8c52c175ac69260ce7`
stayed frozen. Only `esp32_sdio.ko` in the rootfs at `0x810000`
changed. why2025 0015 was not added.

### 29.1 Unique hosted cuts

| Patch | Change | What it cannot see |
|---|---|---|
| 0015 | Skip post-assoc `CMD_GET_TXPOWER` (0xF) and `CMD_STA_RSSI` (0x1B) | A wedged data path |
| 0016 | Skip SET_IP(0) and duplicate SET_IP; re-kick `cmd_work` only on timeout | Whether 0xD must occur during 4-way |
| 0017 | On `INT_ST==0`, dispatch RAW BIT 23 instead of writing `CLR_KNOWN` | AP never sending msg3 |
| 0018 | When RAW is nonzero without BIT 23, ACK that RAW; never ACK unseen BIT 23 | Unnamed bits that are not W1C |
| 0019 | Log TX dequeue and slave-buffer drops; no ACK or command change | Air after a successful CMD53_TX |

Do not skip SET_MCAST. Do not assign Linux IPv4 before both ADD_KEY
responses. BusyBox `ifconfig IP netmask` is forbidden: Down/Up queues
SET_IP(0). USB unplug must be >= 5 s; a P4 reset does not power-cycle
C6.

### 29.2 Cold 4-way

Two independent POWERON_RESET runs after 0018, USB absent 7.1 s and
7.8 s:

- `empty-raw-ack raw=0x004cf800` (BIT 23 clear), then msg3 `00 97`
- two unique ADD_KEY `08 02`
- no boot `status=0x00000000` storm
- Linux address `10.255.10.199/24`

SET_IP (0xD) still hits the 5 s host wait. A late `0d 02` may follow
as `Command response not expected=13`. That command stores the STA IP
for C6 WoW ARP only; it is not a data-TX gate.

### 29.3 DUT-to-AP ping (0019 ko)

0018 then queued Linux TX (wlan0 TX 4 then 7) with **no** `CMD53_TX`
after `ip addr add`. Gateway ARP stayed incomplete
(`00:00:00:00:00:00`). Hypothesis A (AP client isolation alone) is
false for that cut: DUT ping of `10.255.10.1` also failed.

0019 (`esp32_sdio.ko`
`23b7e07a2122f33ad08a0a5828330a7ea06968664dc95f476b20678cb779c014`,
squashfs
`cb8b8b4ec44109eee4ac4abd5e92875baae89eedf65d46d0a3124f5082ee9d89`)
on a 9.0 s unplug:

- `tx_nobuf=0`
- `TX dq` 13 then 17, `CMD53_TX` 12 then 16 (ping window included)
- `10.255.10.1 is alive!`, `DUTGWPING:0`
- gateway ARP `78:8c:b5:cb:02:44` complete
- late `0d 02` after the 0xD timeout, as in the first 0018 4-way

inetd Dropbear started (`SSHSTART:0`). Host ping from
`10.255.10.146` was 1/4 with a dest-unreach; SSH timed out. That is
STA-to-STA / AP isolation, not “C6 never transmitted.” fifo/nc was
not used.

### 29.4 Evidence files

| File | Role |
|---|---|
| `easystick-p4-wifi-gwping-0018-4way-ok-setip-late-20260816.log` | 0018 4-way; SET_IP late ACK; harness stopped before gw ping |
| `easystick-p4-wifi-gwping-0018-noping-incomplete-arp-20260816.log` | 0018 4-way; no CMD53_TX after address; gw ARP incomplete |
| `easystick-p4-wifi-gwping-0019-dut-gw-ok-20260816.log` | 0019 4-way; DUT gw ping OK |
| `easystick-p4-wifi-ssh-0019-live-20260816.log` | inetd started; SSH TimeoutError |

All under `C:\Users\developer\tmp\`. Issue #6 M2/M3 remain open:
DHCP/DNS/NTP, reconnect soak, and key-authenticated SSH from a
peer STA are not claimed.

## 30. Review split: ACK TOCTOU, SSH listen, directional evidence

External review of `d4343e9` (2026-08-16) accepted 0018's nonzero RAW
ACK of `0x004cf800` / `0x004cf000` as hardware-correct (ESP32-C6
SLCHOST bits 11–22 are R/WTC). It rejected treating AP isolation as
proven from a single 1/4 Windows ping, and found two source defects to
fix before the next soak:

1. **0020** — after `ESP_CONTEXT_RX_READY`, RAW==0 must not write
   `ESP_SLAVE_INT_CLR_KNOWN` (still includes BIT 23). ACK
   `CLR_KNOWN & ~RX_NEW_PACKET` instead; when RAW==0 also sample
   `PACKET_LEN` and promote BIT 23 if it advanced past
   `context->rx_byte_count`. Order: INT_ST → INT_RAW → PACKET_LEN → ACK.
2. **S85easystick-ssh** — do not require `wlan0` (or `ethsta0`) before
   starting inetd. Listen on `0.0.0.0:22` as soon as Dropbear's host
   key exists. `SSHSTART:0` is not listen proof; use `netstat -lnt` and
   `dbclient -y pi@127.0.0.1`. Root login stays disabled (`-w`).
3. **m3-lab diagnostics** — `BR2_PACKAGE_LIBPCAP` / `TCPDUMP`, Dropbear
   client, BusyBox `arping` / `netstat` / `nc`. Production `m3` stays
   client-less and without tcpdump.

SET_IP (`0xD`) remains noise for the connectivity baseline: after both
ADD_KEY responses, set Linux IPv4 and **do not** send SET_IP for the
DUT→gateway 100-ping gate. Reintroduce WoW ARP storage only after that
gate is stable.

### 30.1 Next verification gate (Image/DTB/C6 frozen)

Do not change SDIO clock, descriptor count, Image, DTB, or C6. Flash
only the rebuilt `esp32_sdio.ko` (0020) and a rootfs that carries the
SSH/tcpdump fixes.

1. DUT: `dbclient -y pi@127.0.0.1`
2. DUT → gateway: `ping -c 100 -W 1 10.255.10.1` (this port's STA
   iface is `wlan0`; prefer `ip`/`ifconfig` to confirm the name)
3. AP or wired host → DUT: 20 ping
4. Windows Wi-Fi STA → DUT: 20 ping; save the **full** `Reply from`
   source address on every dest-unreach
5. Windows → TCP/22 (`Test-NetConnection` / `ssh -vvv pi@…`)
6. Ten cold boots, each with a 100-ping DUT→gateway run

While (4)/(5) run, DUT tcpdump:

```text
tcpdump -ni wlan0 -e -l 'arp or icmp or tcp port 22'
```

Interpret ARP/SYN visibility per the review table (isolation vs RX stop
vs listen vs Dropbear auth). Prefer stage counters over new hot-path
`esp_info()`; 0020 only logs empty-safe-ack / empty-pktlen-rx on the
existing empty-ISR cadence.

