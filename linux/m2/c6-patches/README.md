# C6 application patches (EasyStick)

Patches here are applied to the **disposable** `network_adapter` copy created
by `build-c6.sh` after `cp -a`. The locked `vendor/esp-hosted` submodule is
never modified.

## Active: `0001-easystick-h2c-tcp22-sig-observe.patch` (0023-A2)

- Intent: correlate P4 `ES_TX` TCP/22 payload packets with C6 H2C arrival and
  `esp_wifi_internal_tx()` OK using a shared 64-bit signature.
- Scope: `process_rx_pkt()` STA DATA path only for IPv4 TCP **source port 22**
  with `payload_len > 0`. Other DATA frames keep the original single
  `esp_wifi_internal_tx()` call.
- Signature (offline-identical from P4 `ES_TX` `seq`/`plen`/`flags`):
  `sig64 = (tcp_seq << 32) | ((plen & 0xffff) << 16) | (flags & 0xff)`
- Unique table (~16) in RAM; aggregates: `H2C_COUNT`, `H2C_XOR64`,
  `WIFI_OK_COUNT`, `WIFI_OK_XOR64`, `LAST_SEQ`, `LAST_PLEN`.
- **Does not** set `WIFI_PS_NONE`, does not retry TX, does not notify
  `send_task`. No SSH ciphertext storage.
- Publish: 1 Hz snapshot to SDIO slave *byte* positions 32..63 (host
  `ESP_SLAVE_SCRATCH_REG_8`..15 at +0x9C) plus one `H2C_OBS` `ESP_LOGI`
  line. ABI v3 magic `0x0B23`.
- Keep `NG-1.0.6.0.1` (`PROJECT_REVISION_PATCH_2 = 1`).
- Identify the image by `network_adapter.bin` SHA-256 and this patch SHA-256.

## Disabled: `disabled/0001-easystick-h2c-echo-reply-observe.patch` (0022)

ICMP Echo-Reply mask observer (ABI v2 magic `0x0B22`). Superseded for the
TCP/22 correlation shot; retained for history.

## Disabled: `disabled/0001-easystick-disable-wifi-modem-sleep.patch` (0021)

`WIFI_PS_NONE` A/B was rejected: VALID_ASSOC + PS_NONE still showed ~22 s
reply bursts. Kept for history; not applied while it sits under `disabled/`.

## Build

```text
docker run --rm \
  -v <firmware>:/firmware \
  -v <out>:/out \
  -w /firmware/linux/m2 \
  espressif/idf:v5.5.3 \
  bash -lc './build-c6.sh --vendor /out'
```
