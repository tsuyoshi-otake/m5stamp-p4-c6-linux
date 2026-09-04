# C6 SDIO slave build evidence — 2026-08-10

Status: **build-only; not flashed**.  This is the ESP32-C6
`network_adapter` image intended for the Stamp AddOn C6 SDIO link.  It does
not prove that the attached AddOn is provisioned, that its reset/recovery path
is reachable, or that the P4 host can enumerate `wlan0`.

## Locked inputs

- ESP-Hosted-NG: commit `8626b42fd3f9eb5a1ccb5daea481f0d8d32b1685`
- ESP-IDF: commit `2c211b236707889e8400c4dc5644dd5c4ee071e0e`
- Target: ESP32-C6, SDIO slave transport
- Build entry point: `linux/m2/build-c6.sh`
- Output: external Docker volume `easystick-p4-c6-out`, not Git

The build staged the ESP-Hosted-NG C6 Wi-Fi library set and ROM patch in the
disposable IDF checkout.  The stock IDF checkout is restored by the script's
exit trap.  `CONFIG_ESP_SDIO_HOST_INTERFACE=y` is present and the SPI host
transport is not selected.

## Artifacts

| Artifact | Size | SHA-256 |
| --- | ---: | --- |
| `bootloader.bin` | 22,448 bytes | `d43c515ea3c079a08a661a83df713673c6acb541c9780d4d825a0d6da8758bff` |
| `partition-table.bin` | 3,072 bytes | `298db510b83c7e14e24785ee96dfb8d5552affb2fecdadbec2764559653bec5f` |
| `network_adapter.bin` | 1,039,040 bytes | `2ac39933c84c7688fb5d251ffc01bd15e53b665a272b6053dedca448e40e1827` |
| `ota_data_initial.bin` | 8,192 bytes | `7d2c7ac4888bfd75cd5f56e8d61f69595121183afc81556c876732fd3782c62f` |

## Write gate

The AddOn has no independently detected USB/serial connector in the current
setup.  Do not run `esptool` against the P4's COM10 and assume it will program
the C6; that would target the wrong chip.  The [official ESP-Hosted P4
reference](https://github.com/espressif/esp-hosted-mcu/blob/main/docs/esp32_p4_function_ev_board.md)
says its C6 is normally factory-preflashed and can be updated by slave OTA;
initial serial flashing requires an ESP-Prog/UART fixture.  The
AddOn schematic exposes C6 UART TX/RX, reset, and boot-strap test signals, but
their physical accessibility on this assembled carrier has not been proven.
Treat the factory image as the first M2 compatibility candidate and capture
the SDIO version/handshake before attempting an update.  If an update is
needed, identify and document the UART/boot-strap or host-OTA route first.
Keep this image separate from the P4 M2/M3 flash candidates and retain the
stock P4 readback outside Git.

## Later delivery note

This record describes the standalone C6 build and its direct-flash gate.  A
byte-identical `network_adapter.bin` was subsequently delivered through the
verified P4 ESP-Hosted host-OTA path; the later P4 diagnostic runs did not
rewrite the C6 image.
