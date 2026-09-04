# SSH TX ledger capture — 2026-08-25

Status: **PASS — P4-only observer capture; C6 unchanged**

This is one controlled observation of the SSH-channel failure. It is not an
A/B behavior comparison. The TX ledger changes logging only; no C6 image or
C6 write was used.

## Fixed target and image

- Target P4: `COM10`, MAC `e8:f6:0a:e2:5e:73`
- Target IPv4: `10.255.10.161`
- Profile: `m3-lab`
- `EASYSTICK_SDIO_FORCE_PIO=0`
- `EASYSTICK_IDMAC_NONCOHERENT_RING=1`
- `EASYSTICK_ESPHOSTED_DISABLE_0010=1`
- `EASYSTICK_ESPHOSTED_TX_LEDGER=1`
- `EASYSTICK_ESPHOSTED_DIAGNOSTICS=0`
- `EASYSTICK_SSH_LEDGER=0`
- `EASYSTICK_DW_MMC_CMD53_ERR_PROV=0`
- `EASYSTICK_CMD53_RETENTION_BB=0`

The candidate artifacts and complete build verification are recorded in
[`ssh-tx-ledger-build-2026-08-25.md`](ssh-tx-ledger-build-2026-08-25.md).
The preserved stock 16 MiB readback remained the flash recovery control:

```text
C:\Users\developer\tmp\easystick-p4-stock-20260809-esptool481-full-v2.bin
bytes 16777216
sha256 229459f251eaf6222f0c07968702d72a3818e520da1788ce465027d969020c24
```

`flash-candidate.ps1 -AllowCandidateWrite` completed the P4-only write and
byte verification. Its log reported `verify OK` for the partition table,
boot-shim, `Image`, `rootfs.squashfs`, and DTB. No C6 operation was performed.

## Reset and boot controls

The failed reset/capture attempts are retained as capture-method controls, not
as firmware results:

| Capture | Result |
| --- | --- |
| `uart-tx-ledger-boot-2026-08-25.bin` — 0 bytes, SHA-256 `e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855` | **FAIL / invalid boot evidence**: the existing RTS-only collector did not leave the P4 ROM wait state |
| `uart-tx-ledger-boot-usbjtag-2026-08-25.bin` — 193 bytes, SHA-256 `b230103e5a2aea54a8647af2e2a07d48cc2b6621ba6381c34c836aaa917bcf91` | **FAIL / reset control**: ROM reported `boot:0x4 (DOWNLOAD...)` |
| `uart-tx-ledger-boot-esptool-reset-2026-08-25.bin` — 193 bytes, same SHA-256 | **FAIL / reset control**: the direct `USBJTAGSerialReset` path also selected ROM download |
| `uart-tx-ledger-reset-probe-2026-08-25.bin` — 15,542 bytes, SHA-256 `6edf5dbec24d04e69474c0668200b6d83e0d3679038d5c261fa16174c9a88faf` | **PASS**: esptool `HardReset` reached Linux, Wi-Fi `10.255.10.161`, and Dropbear |

The collector was corrected to use the esptool-compatible HardReset RTS/DTR
update. A 30-second verification capture,
`uart-capture-reset-fixed-2026-08-25.bin`, produced 15,527 bytes with
SHA-256 `d98a5f5b7adc72a9f0f12bb6ed9cbc5645c5f643c193021d1dbf8e05bfb11fe1`,
first byte at 2.103 seconds, and `reset_strategy` set to
`esptool-HardReset-compatible-RTS-DTR`.

The successful probe showed `boot:0xc (SPI_FAST_FLASH_BOOT)`, Linux 6.18.35,
the noncoherent IDMAC ring, `wlan0`, and the password-enabled SSH service.
The final ledger capture was therefore armed passively without another reset.

## Controlled SSH reproduction

UART was captured before the SSH operation, with no remote command sent:

```text
uart-tx-ledger-ssh-2026-08-25.bin
bytes 30164
sha256 1c392541abb5a78fe7ed5fb64a3eb4046b221463eb9ea449c9abb989ed5e7472
```

The host-side timeline is:

```text
ssh-tx-ledger-channel-repro-passive-2026-08-25.json
bytes 1741
sha256 162d5df301818e604bc8347be41cb8267cf6a66d053062e678b1d298e5f3b044
```

Observed events:

```text
BEFORE_TCP22=True
SSH_AUTHENTICATED elapsed=4.047
CHANNEL_OPEN elapsed=4.391
CHANNEL_RX bytes=258 elapsed=4.594
CHANNEL_STATE_AFTER_5S active=1 closed=False
AFTER_CLOSE_TCP22=False (timeout)
```

This reproduces the network loss after an authenticated interactive channel.
It does not prove that the client-side channel close itself is causal; the
device-side reset follows in the UART stream.

## Ledger result

The raw capture contained 463 decoded lines. Removing 142 exact consecutive
duplicates left 321 lines for stage analysis.

Traces 1 through 9 all completed this sequence:

```text
NETDEV_XMIT
ENQUEUE_OK
DEQUEUE
CREDIT_OK
CMD53_ATTEMPT
CMD53_CLAIM_ENTER
CMD53_MEMCPY_ENTER
CMD53_MEMCPY_DONE ret=0
CMD53_CLAIM_LEAVE ret=0
CMD53_OK ret=0
```

The next server-to-client frame was:

```text
NETDEV_XMIT dport=55516 seq=3403645058 ack=25862119 plen=48 flags=0x18
ENQUEUE_OK  dport=55516 seq=3403645058 ack=25862119 plen=48 flags=0x18
DEQUEUE     dport=55516 seq=3403645058 ack=25862119 plen=48 flags=0x18
```

No `CREDIT_OK`, `CREDIT_NO_BUFFER`, `TOKEN_READ_ERR`, or
`CMD53_ATTEMPT` followed that `DEQUEUE`. The next reset marker was:

```text
rst:0x7 (HP_SYS_HP_WDT_RESET),boot:0xc (SPI_FAST_FLASH_BOOT)
```

## Analysis conclusion

The first observed failing boundary is:

```text
DEQUEUE → is_sdio_write_buffer_available()
```

The ledger places `DEQUEUE` before the credit check and places
`CREDIT_OK`/credit error markers after it. In this driver,
`is_sdio_write_buffer_available()` obtains the token count through
`esp_slave_get_tx_buffer_num()`, which calls
`esp_read_reg(ESP_SLAVE_TOKEN_RDATA, ...)`. In the selected series, register
access is the byte-wise CMD52 path from patch `0012`; the bulk CMD53 path is
only reached after the credit check.

Therefore this capture supports:

**The P4-side SSH TX path wedges while checking the C6 TX-credit/token
register, before the failing frame enters CMD53 bulk memcpy. The MWDT then
recovers the P4.**

It does not identify the exact CMD52 byte, controller instruction, or C6
response that blocks. The nine preceding `CMD53_OK` frames specifically mean
that this shot does not support calling `sdio_memcpy_toio()` the first failing
operation. A byte-level register-read ledger or the existing bounded WDT
crash-capsule path would be the next diagnostic only if that remaining
distinction is required.

## Evidence boundary

The UART collector stores raw bytes but does not timestamp each byte against
the host timeline. The stage ordering and reset marker are strong evidence of
the failing path, while exact host-to-UART latency remains unmeasured.
`printk(KERN_EMERG)` instrumentation can also perturb timing. The result is
an observed failure boundary under the fixed DMA/noncoherent configuration,
not a claim that logging is behavior-neutral at instruction timing.
