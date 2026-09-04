# M2: Stamp AddOn C6 over SDIO

M2 is the first Linux image profile that attempts to expose the attached
Stamp-AddOn C6 as `wlan0`.  It is intentionally separate from the proven M1
boot image: a bad SDIO/C6 image must not remove the serial recovery path.

## Hardware contract

The AddOn uses the P4 SDMMC controller's flexible Slot 1 GPIO matrix:

| Function | P4 GPIO | C6 AddOn signal |
| --- | ---: | --- |
| reset/EN | 42 | C6 reset (active-low) |
| CLK | 43 | SDIO CLK |
| CMD | 44 | SDIO CMD |
| D0 | 45 | SDIO DAT0 |
| D1 | 46 | SDIO DAT1 |
| D2 | 47 | SDIO DAT2 |
| D3 | 48 | SDIO DAT3 |

The M5Stack wiring and C6 slave image must be verified against the actual
AddOn revision before a flash.  CMD and DAT0..3 require pull-ups; the image
enables the P4 internal pulls as a diagnostic aid, but that is not a substitute
for the external pull-ups required by the ESP-Hosted electrical contract.

The P4 host is limited to 20 MHz during bring-up.  DMA is deliberately not
used until the P4 descriptor/cache behavior is measured; the first kernel
profile forces the generic DW-MMC driver to PIO where supported.

## Software pieces

* Linux 6.18.35 with the P4 `dw_mmc` card-number/clock-divider fix.
* The standard ESP-Hosted-NG SDIO Linux host module from the locked vendor
  checkout.  It is built out-of-tree and installed as `esp32_sdio.ko`.
* An M2-only boot-shim path (`boot-shim/main/m2_sdmmc.c`) that clocks the
  controller, pulses C6 reset GPIO42, and routes Slot 1 through the GPIO
  matrix.  It is selected only when `EASYSTICK_BOOT_PROFILE=m2` is set while
  building the ESP-IDF shim; the M1 shim remains unchanged.
* ESP32-C6 `network_adapter` slave firmware built from the same locked
  ESP-Hosted-NG revision and ESP-IDF 5.5.3.  `build-c6.sh` creates this image
  without flashing it. The C6 image is not copied into the P4 flash image; it
  has its own recovery/flash procedure. The script stages the special C6
  Wi-Fi library set and ROM patch shipped by ESP-Hosted-NG; a stock ESP-IDF
  Wi-Fi library is not a substitute for that set.  The AddOn has no detected
  USB/serial port in the current setup, so first try the factory-preflashed
  slave.  Update it only through a verified host-OTA or a separately wired
  UART/boot-strap fixture; COM10 programs the P4, not the C6.

`build-m1.sh --profile m2` is the P4 build-only command.  It accepts either the
external source cache or `--vendor`; the module source is intentionally taken
from the locked submodule rather than vendored into this repository. Use
`build-c6.sh` separately for the C6 slave image.

For a temporary first-board SSH smoke test, use `build-m1.sh --profile
m3-lab`. That profile layers Dropbear/BusyBox `inetd` on M2 and intentionally
ships the Raspberry-Pi-style `pi`/`raspberry` lab credential. Set
`EASYSTICK_WIFI_SSID` and `EASYSTICK_WIFI_PSK` in the build container to
generate a private Wi-Fi file in the external output. The production key-only
profile remains under `../m3/`.

## Bring-up order

1. Build and inspect the M2 image; verify the DTB has Slot 1 and the module
   build uses the locked ESP-Hosted commit.
   Run `tools/verify-images.py --layout m2/flash-layout.json` with the
   generated Image, rootfs, and DTB. The M1 candidate map cannot hold the
   larger networking kernel.
2. Keep a stock readback and the M1 flash offsets available.  Verify that the
   factory C6 slave is present and record its ESP-Hosted handshake/version.
   Do not use the P4 COM10 port as a C6 programmer.
3. If the factory slave is incompatible, flash the C6 image through a
   separately verified OTA or UART/boot-strap recovery path; otherwise leave
   the C6 untouched.  Then flash P4 boot shim/kernel/rootfs/DTB separately.
4. Capture the complete boot log.  Require `mmc1`, SDIO function discovery,
   `esp32_sdio`, and `wlan0` before attempting association.
5. Run scan, WPA2 association, DHCP/DNS, reconnect, and the 30-minute soak.
   Only then enable the M3 key-only Dropbear profile and test SSH. The
   `m3-lab` image may be used for the first private-network handshake, but its
   password must be changed or the image replaced before any wider test.

No Wi-Fi credentials, private keys, or captured serial data belong in Git.
The P4 has no general-purpose EEPROM; raw logs remain on the host with a
sidecar SHA-256 manifest.

## Why this bring-up is hard (Issue #6)

Missing public docs matter, but they are not the whole story. The hard part
is a **non-standard stack** plus **state-dependent, low-reproducibility
failures** that cross several layers.

### No complete reference for this exact combination

WHY2025 Linux is invaluable as a proof that native ESP32-P4 NOMMU Linux can
boot, but its author marks it as a proof of concept that is not
upstream-ready and not battle-tested
([why2025-linux](https://git.hubp.de/mrbreaker/why2025-linux)). EasyStick
further changes the C6 path from that SPI-oriented reference to **GPIO42–48
SDIO**.

Nearby public pieces exist separately:

| Piece | What exists |
| --- | --- |
| Linux + ESP-Hosted + SDIO | ESP-Hosted-NG ships `esp32_sdio.ko` and documents C6 SDIO ([setup.md](https://github.com/espressif/esp-hosted/blob/master/esp_hosted_ng/docs/setup.md)) |
| ESP32-P4 + C6 + SDIO | Official examples run ESP-IDF / ESP-Hosted-MCU on P4, including EV-board iperf over SDIO ([esp-hosted-mcu](https://github.com/espressif/esp-hosted-mcu)) |
| ESP32-P4 + native Linux | WHY2025 NOMMU port (above) |

What does **not** exist as a finished reference is the full chain used here:

```text
ESP32-P4
  → native RV32 NOMMU Linux
  → EasyStick P4 SD/MMC Linux host path
  → ESP-Hosted-NG Linux host
  → SDIO
  → ESP32-C6
  → Wi-Fi
```

That missing complete example is the largest single difficulty.

### Layers that look healthy while packets vanish

A broken SDIO path often shows CMD53 errors, `wlan0` down, association
failure, kernel errors, or a C6 crash. Issue #6 instead saw:

```text
Linux boots
wlan0 appears
Wi-Fi association succeeds
DHCP sometimes works
ping sometimes works
Linux reports Echo Replies generated
C6 stays alive
yet packets disappear
```

The 0022 closure run later recorded a fully healthy path:

```text
ΔInEchos=20 → ΔOutEchoReps=20 → C6 H2C=20 → C6 Wi-Fi OK=20 → Pi replies=20
```

An earlier run in the same neighbourhood lost 10 of 20. That pattern is not
“always-broken code”; it is **a system that fails only in some internal
states**. That raises debug cost by several steps.

### SDIO is a long asynchronous pipeline

SDIO is not “write() and it arrives”. The path under investigation is:

```text
Linux network stack
  → ESP-Hosted netdev
  → skb queue
  → TX credit / token
  → CMD52 / CMD53
  → DW-MMC
  → SDIO bus
  → C6 slave queue
  → esp_wifi_internal_tx()
  → 802.11
```

Interrupt, credit, queue, DMA, shared registers, and read/write pointers all
interact asynchronously. Espressif’s own P4+C6 SDIO reports still include
init failures, slave watchdog bootloops, RX/TCP stalls, and memory-related
issues ([esp-hosted#740](https://github.com/espressif/esp-hosted/issues/740)
and related). So `OutEchoReps=20` only proves Linux *generated* twenty
replies — not that twenty reached the C6. That is why observe patches
**0022** (C6 H2C / `esp_wifi_internal_tx` ledger) and planned **0023** (P4
host stage ledger) exist; see [`c6-patches/README.md`](c6-patches/README.md).

### Telemetry had to be invented while debugging

Stock drivers do not record per-ICMP-seq facts such as “left Linux”,
“enqueued”, “got SDIO credit”, “CMD53 OK”, “C6 accepted”. For normal
products that detail is unnecessary. Here the work built measurement
apparatus while chasing the bug (`0018`…`0022`, then `0023`). After 0022,
frames that reach C6 H2C and return `esp_wifi_internal_tx=ESP_OK` are
strongly correlated with Pi receipt; loss before that stage remains the
open centre when the intermittent path reappears.

### Difficulty ranking (working estimate)

| Factor | Contribution |
| --- | ---: |
| Almost no reference for P4 native NOMMU Linux + C6 SDIO Linux host | ★★★★★ |
| State-dependent / unstable reproduction rate | ★★★★★ |
| Long failure boundary: Linux → SDIO → C6 → Wi-Fi | ★★★★★ |
| Insufficient stock telemetry | ★★★★☆ |
| ESP-Hosted/SDIO still has rough edges | ★★★★☆ |
| Cross-cutting MCU / Linux / ESP-IDF / kernel / SDMMC skill set | ★★★★☆ |

So the accurate summary is not “search failed and cost time”. It is:
**existing docs cover boot, ESP-Hosted, C6-as-coprocessor, and SDIO
separately; the last 20–30% — why Echo Replies drop for tens of seconds on
this exact port, SDMMC path, ESP-Hosted-NG revision, and C6 firmware — has
no published answer.** That remainder is research and new porting work.
Issue #6 evidence (including 0022/0023 when complete) is itself becoming
the missing public reference for the next **ESP32-P4 Linux + C6 SDIO**
effort.
