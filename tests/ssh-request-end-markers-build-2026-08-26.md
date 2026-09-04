# SSH request-end marker build — 2026-08-26 (v6)

## Scope

This is a build-only observation update for the Stamp-P4 SSH disconnect
investigation. It adds four generation-tagged retention markers inside
`dw_mci_request_end()` and repairs the generator's previously silent BB1
replacement miss. No P4 flash, COM10 reset, or C6 erase/write was performed.

The new request-end fields are:

| Field | Observation point |
|---|---|
| `stage_request_end_enter` | entry to `dw_mci_request_end()` |
| `stage_request_end_before_next` | immediately before a queued request starts |
| `stage_request_end_after_next` | immediately after that start returns |
| `stage_request_end_idle` | after the no-queued-request idle state is set |

The original focused `mrq` pointer is used for matching, so clearing
`host->mrq` during queue handoff does not suppress these markers.

## Correction to the previous runtime interpretation

The generated 0052 patch previously declared `bb_gen` but did not contain the
BB1/BB2/BB3 call sequence. The generator tested `old_end` but then replaced
`old_end + "\n}"`; the vendor source has the closing brace immediately after
`spin_lock(&host->lock);`, so that second replacement silently matched zero
bytes.

The generator now:

1. matches `old_end + "}"` exactly once;
2. fails if the BB1 call is absent after replacement; and
3. emits BB1, BB2, and BB3 together with the four request-end markers.

Therefore the previous boundary captures with `stage_end=0` cannot be used to
claim that execution stopped inside `dw_mci_request_end()`. The next runtime
shot is the first one that will contain a real BB1 seal and the new split.

## Build result

Result: **PASS**.

- Build: `m3-lab`, boot-shim profile `m2`, exit code `0`.
- Build interval: `2026-08-26T05:37:01Z` to `2026-08-26T05:55:46Z`
  (18m44s).
- DMA reachability conditions: `EASYSTICK_IDMAC_NONCOHERENT_RING=1` with
  `EASYSTICK_CMD53_BB_ALLOW_IDMAC_NC=1`.
- Other retention conditions: `FORCE_PIO=0`, descriptor invalidation `=0`,
  RX descriptor diagnostic `=0`, TX/TCP/SSH ledgers `=0`, diagnostics `=0`.
- Retention PA: `0x50108080u`.
- `nm -S` result: BB size `0x120` (288 bytes); `.rtc_noinit` range
  `[0x50108080, 0x501081a0)`.
- Static layout assertion, exact-size gate, and WDT crash-capsule fail-closed
  gate: **PASS**.
- Rendered-patch gate, including BB1/BB2/BB3 and all four request-end fields:
  **PASS**.
- `final_shot_manifest.py verify --require-shot-c`: **PASS**.
- Export `SHA256SUMS.txt`: all 23 entries: **PASS**.
- Final manifest records version `6`, size `288`,
  `EASYSTICK_IDMAC_NONCOHERENT_RING=1`, and `shot_c_allowed=true`. This is an
  artifact gate only; the image was not flashed.

## Key artifact hashes

Export directory:

`D:\Users\Developer\easystick-tmp-20260820\easystick-p4-wdt-0054-ssh-capsule-20260825\request-end-v6\`

| Artifact | SHA-256 |
|---|---|
| `boot-shim.bin` | `aed25285b2f7a352de48b481e53ad816cf09b36a60e974c9c5e5792593352061` |
| `Image` | `8a649741212ada6b0dfb089742ade276ac8036c2dc54593fadadcad462a98e65` |
| `rootfs.squashfs` | `2e77e47b92579c4aece56019e15fff141fba2f10fd52422eb8663561f52f9c8d` |
| `vmlinux` | `cfc715e5a526be08f30b3eb97d96d0b70ff6e84dcba3885b1d48a0f02a81978b` |
| `rendered-patches/0052-easystick-dw-mmc-cmd53-retention-bb.patch` | `0377db1b5caffcb2a7123890cdb5465fa9f2ea9c57af9162a08f70c93843cd55` |
| `rendered-patches/0053-easystick-mmc-cmd53-post-bb.patch` | `de13f92137881db059e3db2f50c737a696d74c4ebc9e0e41d07cab061711afbc` |
| `rendered-patches/0054-easystick-wdt-crash-capsule.patch` | `3d8cd4dd6c8331810773d484976f27519620b765160a5ae1392dfe29f0663c5e` |
| `rendered-patches/0026-easystick-cmd53-retention-bb6.patch` | `9ae73f1c5271c663ad8a4d88dae8b3cdeb45c6166d5a54e68977be43286952bb` |

## Runtime status

No runtime classification is claimed for this build. The previous failing
UART controls remain:

- `boundary/uart-boundary-ssh-2026-08-26.bin`: **FAIL**, WDT reset and
  `CRASH_CAPSULE empty/invalid`.
- `boundary/uart-boundary-exec-2026-08-26.bin`: **FAIL**, same WDT reset and
  no capsule.
- `uart-a2-after-hard-reset.bin`: WDT capsule positive control **PASS**, but
  it is not SSH-failure evidence.

The next runtime step, if separately authorized, is one P4-only flash followed
by one UART-first SSH reproduction. C6 must remain unchanged.
