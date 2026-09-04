# CMD53 retention black-box (Experiment C / 0052 + 0053 + v6 boundaries)

## Design locks

1. Storage: boot-shim `RTC_NOINIT_ATTR` → `.rtc_noinit`.
2. Kernel PA from `nm -S` of the **same** boot-shim ELF that is flashed.
3. Profile map: `m2|m3|m3-lab → boot-shim m2`, `m1 → m1`.
4. Torn-safe: `commit=0 → barrier → payload → barrier → gen^seq^XOR`.
5. Generation-tagged stages; dump prints `NA` for stale payload fields.
6. **FOCUS_ARG** `0x97ec0000` only (Boot B hang CMD53: write/0x1f600/512).
7. Wrapper **forces** DMA/quiet: `FORCE_PIO=0`, IDMAC A/B=0, ledgers=0.
   Reachability may set `EASYSTICK_CMD53_BB_ALLOW_IDMAC_NC=1` + NC=1.
8. Retention builds always `linux-dirclean`; profile identity includes PA +
   rendered-0052/0053 SHA.
9. Flash only with `cmd53-bb-final-shot-manifest.json` where
   `shot_c_allowed=true` (non-selftest rebuild), verified by
   `final_shot_manifest.py verify --require-shot-c`.

## 0053 post-`mmc_request_done` breadcrumbs (no hot-path printk)

| Tag | Stage field | Seal point |
|-----|-------------|------------|
| BB1 | `stage_end` | immediately before `mmc_request_done()` |
| BB2 | `stage_bb2` | immediately after `mmc_request_done()` returns |
| BB3 | `stage_bb3` | `dw_mci_request_end` return (`host->state` / pending) |
| BB4 | `stage_bb4` | after `mmc_wait_for_req()` returns |
| BB5 | `stage_bb5` | after `sdio_memcpy_toio()` returns |
| BB6 | `stage_bb6` | after `esp_write_block()` returns |

Dump line includes `end= bb2= bb3= bb4= bb5= bb6=` bits.

Classification: last sealed BB tag before natural WDT is the fault boundary.
**FORCE_PIO stays deferred** until clean retention shows DTO/data-phase fault.

## v6 pre-completion boundaries (no hot-path printk)

The v6 record keeps the crash capsule at `PA + 0x80` and appends six
generation-tagged stages after the capsule:

This is an intentional layout-version bump from the earlier `0x110`-byte
record. Do not mix a v5 producer or consumer with this `0x120`-byte record.

| Stage field | Seal point |
|-------------|------------|
| `stage_idmac_complete_enter` | immediately before `dma_ops->complete()` |
| `stage_idmac_complete_exit` | immediately after `dma_ops->complete()` |
| `stage_bh_enter` | entry to `dw_mci_work_func()` |
| `stage_data_complete_enter` | immediately before `dw_mci_data_complete()` |
| `stage_data_complete_exit` | immediately after `dw_mci_data_complete()` |
| `stage_irq_exit` | final return path of `dw_mci_interrupt()` |

The boot-shim prints these as `dma_in`, `dma_out`, `bh_in`, `data_in`,
`data_out`, and `irq_out`. An `enter=1` with its matching `exit=0` identifies
the boundary containing the hang; all zero means the IRQ or an earlier path
did not reach these hooks. These are observation-only retention writes and
are not a throughput or recovery change.

The same record also splits the request-end interval:

| Stage field | Seal point |
|-------------|------------|
| `stage_request_end_enter` | entry to `dw_mci_request_end()` |
| `stage_request_end_before_next` | before starting a queued next request |
| `stage_request_end_after_next` | after that next request start returns |
| `stage_request_end_idle` | after the no-queued-request idle state is set |

The boot-shim prints these as `enter`, `before_next`, `after_next`, and `idle`.
The markers use the original focused request pointer, so clearing
`host->mrq` during queue handoff cannot hide the observation. For a queued
request, `before_next=1` with `after_next=0` identifies
`dw_mci_start_request()`; for an empty queue, `idle=1` means the remaining
interval is only the BB1 seal path. These markers add no printk or recovery
behavior.

## CMD52 six-word positive-control marker

The stacktrace/CMD52 quiet control can be followed by a candidate with
`EASYSTICK_ESPHOSTED_CMD52_MARKER=1` and
`EASYSTICK_ESPHOSTED_CMD52_TRACE=0`. The candidate keeps the 0012 byte-wise
register path and adds six independent `u32` words at `BB_PA + 0x120`,
immediately after the fixed v6 record:

| Word | Value | Single `WRITE_ONCE()` store |
|---:|---|---|
| 0 | `MARKER_MAGIC = 0x45534d30` | Linux marker-region magic |
| 1 | `ARMED = 0x45534d31` | Linux arm before SDIO driver registration |
| 2 | `TOKEN_ENTER = 0x45534d32` | matching `reg=0x44,size=4` predicate |
| 3 | `AFTER_46 = 0x45534d33` | after `sdio_readb()` returns for `0x46` |
| 4 | `BEFORE_47 = 0x45534d34` | immediately before `sdio_readb()` for `0x47` |
| 5 | `AFTER_47 = 0x45534d35` | after `sdio_readb()` returns for `0x47` |

The Linux arm runs before `sdio_register_driver()`, so a reset performed
before SSH can prove that the Linux store reaches the intended retention
address and survives reset. The boot-shim clears all six words after dumping
them. `assert_rtc_noinit.py --require-cmd52-marker` fails unless the symbol is
exactly `easystick_cmd53_bb + 0x120` and is six words (24 bytes). The
ESP-Hosted patch is rendered from that same nm-derived PA; an unrendered
`BBDEAD` placeholder is rejected.

The hot path writes no console data, allocates no memory, takes no lock, and
does not read a clock. Each stage has a separate retention word, so an
observed `ARMED` without `TOKEN_ENTER` is meaningful and
`AFTER_46`/`BEFORE_47`/`AFTER_47` cannot overwrite one another. This remains
a last-shot record, not a torn-safe event log.

There is no live userspace reference path in this candidate. First boot and
reset without SSH validates `MARKER_MAGIC`/`ARMED`; then repeat the matching
boot and perform one UART-first SSH reproduction. Only after the wedge, use
the existing P4-only `capture-boot.ps1 -Reset` collector. The boot-shim prints
all six words before clearing them. This controlled reset does not flash,
erase, or write the C6. Do not interpret a marker from a boot that was not
preceded by the matching candidate build and reproduction.

Normal Wi-Fi/ESP-Hosted startup can itself perform a matching
`TOKEN_RDATA` read, so the four stage words are not automatically an
SSH-specific record. The marker-enabled module therefore also exposes a
root-only shot gate:

```text
/sys/module/esp32_sdio/parameters/cmd52_marker_shot
```

Writing `1` to this parameter calls the same kernel
`WRITE_ONCE()` retention helper, clears all six words, and re-arms only
`MAGIC`/`ARMED`. Use it from the ttyGS1 root shell after Wi-Fi/Dropbear is
ready and immediately before starting the passive UART capture and one SSH
command. Do not use `devmem` for this gate: a readback of a userspace physical
mapping is not proof that the boot-shim later sees the store. The candidate's
root-console `/dev/mem` attempt was rejected for exactly that reason.

## Build

```bash
# Positive / torn selftests (shot_c_allowed=false)
./build-cmd53-bb.sh --vendor /out --profile m3-lab --selftest
./build-cmd53-bb.sh --vendor /out --profile m3-lab --selftest-torn

# Canonical wrapper build (the wrapper intentionally forces NC=0).
# For M3-LAB Wi-Fi, use the separate NC=1 build-m1 sequence below.
./build-cmd53-bb.sh --vendor /out --profile m3-lab

# Quiet CMD52 six-word positive-control candidate: first render marker PA.
# build-cmd53-bb.sh --shim-only intentionally keeps its canonical NC=0 contract.
EASYSTICK_ESPHOSTED_CMD52_MARKER=1 \
EASYSTICK_ESPHOSTED_CMD52_TRACE=0 \
EASYSTICK_STACKTRACE_DIAGNOSTICS=1 \
  ./build-cmd53-bb.sh --vendor /out --profile m3-lab --shim-only

# Then build the same NC=1 host condition as runtime2/quiet-control.
EASYSTICK_CMD53_RETENTION_BB=1 \
EASYSTICK_CMD53_BB_ALLOW_IDMAC_NC=1 \
EASYSTICK_SDIO_FORCE_PIO=0 \
EASYSTICK_IDMAC_DESC_INVALIDATE=0 \
EASYSTICK_IDMAC_NONCOHERENT_RING=1 \
EASYSTICK_CMD53_RX_DESC_BYTES=0 \
EASYSTICK_ESPHOSTED_DISABLE_0010=1 \
EASYSTICK_ESPHOSTED_CMD52_MARKER=1 \
EASYSTICK_ESPHOSTED_CMD52_TRACE=0 \
EASYSTICK_STACKTRACE_DIAGNOSTICS=1 \
EASYSTICK_CMD53_BB_SKIP_BR_CLEAN=1 \
EASYSTICK_KERNEL_PATCH_OVERRIDE_DIR=/out/cmd53-bb-rendered-patches \
EASYSTICK_CMD53_BB_PA="$(tr -d '\r\n' </out/cmd53-bb.pa)" \
EASYSTICK_CMD53_BB_ESPHOSTED_MARKER_PATCH=/out/cmd53-bb-rendered-patches/0030-easystick-sdio-cmd52-retention-marker.patch \
  ./build-m1.sh --vendor /out --profile m3-lab

python3 cmd53-bb/final_shot_manifest.py verify \
  --manifest /out/cmd53-bb-final-shot-manifest.json \
  --require-shot-c
```

The final-shot writer records the actual ring mode and requires the exact
retention layout size. For an NC=1 image built by the separate `build-m1.sh`
path, pass `--idmac-noncoherent-ring 1 --bb-size 0x120` when writing the
manifest.

For the six-word marker candidate, use this runtime order: boot and wait for
Wi-Fi/Dropbear, perform a controlled P4 reset without SSH, and require
`magic=0x45534d30` plus `armed=0x45534d31` in the recovery UART. Reboot the
same candidate, use the ttyGS1 root shell to write
`1 > /sys/module/esp32_sdio/parameters/cmd52_marker_shot`, verify the command
returned successfully, close the console, and start `capture-boot.ps1`
without `-Reset`. Reproduce exactly one SSH command while the passive capture
continues, stop after the bounded window, and only then perform the second
controlled P4 reset to retrieve `token_enter`, `after_46`, `before_47`, and
`after_47`. A zero or missing `magic`/`armed` result invalidates the shot and
requires fixing the marker path first; it is not an M1 boundary claim.

## Stacktrace/CMD52 diagnostic image

Set `EASYSTICK_STACKTRACE_DIAGNOSTICS=1` on the separate `build-m1.sh` call
after the `--shim-only` preparation. This keeps the v6 retention layout,
enables `CONFIG_STACKTRACE`, `CONFIG_KALLSYMS`, `CONFIG_KALLSYMS_ALL`,
DWARF4, and frame pointers, and adds the P4-only WDT pretimeout
`dump_stack()` hook.

The ESP-Hosted patch is bounded to 128 events and observes only the
`ESP_SLAVE_TOKEN_RDATA` CMD52 read. Each record is
`ES_CMD52 seq=... stage=... addr=... index=... size=... lock=... value=... ret=...`.
`CLAIM_ENTER` without `CLAIM_DONE` indicates a host-claim wait;
`BYTE_ENTER` without `BYTE_DONE` identifies the CMD52 byte; and
`RELEASE_ENTER` without `RELEASE_DONE` identifies the release boundary.
The trace is intentionally instrumentation-sensitive and is not an SSH
acceptance image. Decode any WDT trace against the matching `vmlinux` and
`System.map` from the same output.

`CONFIG_DEBUG_INFO` can leave DWARF sections in the out-of-tree
`esp32_sdio.ko`. The diagnostic post-build hook strips the rootfs copy of that
module while retaining the unstripped build output for offline analysis.
`physmap-core` exposes this board's 7 MiB `mtd-rom` resource as a 4 MiB map
when no address-GPIO window is present, so the diagnostic build fails closed
if its SquashFS exceeds 4 MiB.

## Canonical 0053 shot (same as Boot B)

cold reset → passive COM10 → association → ping+banner only → stop probes →
one `ssh … '/bin/true'` → no touch → natural WDT → retention dump.
Change from Boot B: breadcrumbs only (0052 BB1–3 + 0053 BB4–5 + esp 0026 BB6).
