# 0051 binary attestation — Experiment B observer (2026-08-21)

## Purpose

Zero `ES_MMC CMD53_ERR` observations are only meaningful if the observer
was compile-in on the shot Image. No positive-control fault injection
(would perturb timing). Binary/string attestation only.

## Artifacts

| Artifact | SHA256 | `ES_MMC CMD53_ERR` | `CMD53 error: arg=` (0010) |
|----------|--------|--------------------|----------------------------|
| `Image-b-errprov` (shot B) | `2EF9030B1F6B37788E767F4BE6F5B7DA935CA0613C6CE2D980344D34D1403C0A` | **1** | **0** |
| Baseline `Image` (flag-off quiet ledger) | `E0029603085CC0341F99FE9C4B44251212E5197A2BE1661FE1AD21718C702154` | **0** | **1** |

Both Images are 6576968 bytes. Shot B Image was produced by applying the
0051 edits onto the already-patched quiet-ledger `linux-custom` tree
(FORCE_PIO=0 / IDMAC noncoherent path), not by inventing a new profile.

## Gate checklist

| Gate | Result |
|------|--------|
| Source tree contains `ES_MMC CMD53_ERR` observer after apply | PASS (docker `linux-custom` + repo `0051-…patch`) |
| Shot Image contains observer format string | PASS (`ES_MMC` count=1) |
| Shot Image lacks superseded 0010 `CMD53 error: arg=` string | PASS (count=0) |
| Baseline Image lacks observer string | PASS (`ES_MMC` count=0) |
| Baseline Image retains 0010 string | PASS (count=1) |
| `build-m1.sh`: `EASYSTICK_DW_MMC_CMD53_ERR_PROV=1` appends only `0051` | PASS (opt-in block; default 0) |
| `build-m1.sh`: flag=0 → 0051 not in patch list | PASS (conditional `if`) |
| flag=1 vs 0: other DW-MMC patch selection unchanged in script | PASS (0051 is additive; does not alter 0008–0014 gates) |

## What this does / does not prove

- Proves the B shot Image carried the error-only capsule and removed the
  0010 hot-path `dev_info` string.
- Does **not** prove that a failed request reached `dw_mci_request_end`
  (zero prints remain compatible with “never completed”).
- Does **not** by itself prove that printk count alone moved the fault;
  Image layout also changed (see Issue wording: instrumentation-sensitive).

## Discarded attempts (not B evidence)

1. **First Image rebuild** — helper inserted mid-`#define`, broke `dw_mci.o`.
   Discarded build attempt; not flashed.
2. **First `/bin/true` probe** — host blocked in `recv_exit_status`; process
   killed before capture finalize. Incomplete shot; do **not** claim same
   root cause. Canonical B shot is the hard-reset re-run
   (`console-b-errprov-true-20260821.bin`).

## B re-run boot / C6 continuity note

Canonical capture opens COM10 after `esptool … hard_reset`, so boot-shim
lines such as `C6 reset GPIO42…` are often already past. The capture **does**
include a fresh Linux SDIO bring-up on the same shot:

- `dw_mmc … Using internal DMA controller`
- `mmc0: new SDIO card at address 0001` (count=2, duplicated tty)
- then SSH AUTH / VFORK / post-exec `CMD53_ATTEMPT` without OK/ERR

That weakens “hung first probe left C6 in a bad EN state carried into B”
as the primary explanation for the B signature, without claiming the
boot-shim GPIO42 pulse itself was observed on this UART window.
