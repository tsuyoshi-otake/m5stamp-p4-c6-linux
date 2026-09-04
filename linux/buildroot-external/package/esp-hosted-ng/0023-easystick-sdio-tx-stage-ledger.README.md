# 0023-A: P4 host TX stage ledger (observe-only)

Test-only markers for ESP-Hosted NG host TX on EasyStick Stamp-P4.

## Intent

Classify where post-auth SSH (TCP **source** port 22, payload > 0) stops on the
P4 host path:

`netdev → enqueue → dequeue → credit → CMD53 (claim / memcpy)`

## Apply

Buildroot series file:

`0023-easystick-sdio-tx-stage-ledger.patch`

`build-m1.sh` applies it only when `EASYSTICK_ESPHOSTED_TX_LEDGER=1`.
The default quiet acceptance image omits it.

This patch contains only the TX ledger. Quiet console and hexdump suppression
remain independently owned by `0024` and `0025`, so the three patches can be
applied together without duplicate hunks.

Header source: `include-easystick_tx_ledger.h` → `include/easystick_tx_ledger.h`

## Markers (`ES_TX`, `printk(KERN_EMERG)`)

Stage markers: `NETDEV_XMIT`, `ENQUEUE_OK`, `DEQUEUE`, `CREDIT_OK`,
`CREDIT_NO_BUFFER`, `TOKEN_READ_ERR`, `CMD53_ATTEMPT`, `CMD53_OK`,
`CMD53_ERR`.

CMD53 split (sport22 only, caller-owned `struct es_tx_cmd53_ctx *tr`):

`CMD53_CLAIM_ENTER`, `CMD53_MEMCPY_ENTER`, `CMD53_MEMCPY_DONE`,
`CMD53_CLAIM_LEAVE`

Every CMD53 stage shares `trace`, `seq`, `plen`, `addr`, `xfer` (and `ret`
when applicable). Filter at the existing `es_tx_parse_skb` caller; do not
use a global `ssh_trace_active` flag.

Optional reasons: `PORT_CLOSED`, `STOP_DATA`, `HOST_SLEEP`,
`TX_PENDING_LIMIT`, `INVALID_LENGTH`.

## Must not

Change C6 firmware, credit/retry policy, IRQ/DMA, Dropbear, BusyBox, WDT, or
PSRAM. Logging only.

## Evidence (2026-08-21)

- Ledger-only: `console-txledger-true-then-id-20260821.bin` — post-VFORK
  `CMD53_ATTEMPT` without OK/ERR.
- Sport22+trace: `console-a-sport22-true-20260821.bin` — same `trace=10`
  reaches `MEMCPY_DONE ret=-110` / `CMD53_ERR` then DUT dies.
- Staging reports under
  `D:\Users\Developer\easystick-tmp-20260820\easystick-p4-m3-lab-quiet-txledger-20260821\`.
