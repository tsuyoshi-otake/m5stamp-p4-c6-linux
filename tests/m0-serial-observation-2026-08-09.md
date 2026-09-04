# M0 serial observation — 2026-08-09

This is a sanitized, read-only observation from the assembled Stamp-P4 target
currently connected through its module USB-C port. It is evidence for the
hardware inventory only; it is not a Linux boot result and it is not a stock
image backup.

## Capture

- Host port: `COM10`, Windows device `USB Serial Device`, VID `303A`, PID
  `1001`.
- Method: opened the port at 115200 baud with DTR/RTS disabled and read the
  existing boot/application log for five seconds. No bytes were written and no
  flash command was run.
- ROM banner: `ESP-ROM:esp32p4-eco2-20240710`.
- Chip revision: `v1.3`.
- Current application: `stampp4-factory-test`, ESP-IDF `v5.4.2-dirty`.

## Observed memory and stock partition map

- SPI flash: 16 MiB, DIO, 80 MHz.
- HEX PSRAM: 32 MiB (256 Mbit); the factory application reported `SPI SRAM
  memory test OK`.
- Current stock partition table (do not reuse as the Linux map without a fit
  review):

  | Label | Offset | Length |
  |---|---:|---:|
  | `nvs` | `0x00009000` | `0x00006000` |
  | `phy_init` | `0x0000F000` | `0x00001000` |
  | `factory` | `0x00010000` | `0x00A00000` |
  | `storage` | `0x00A10000` | `0x00400000` |

## Read-only ROM probe — 2026-08-09

After the original application-log capture, the same connected `COM10` target
was queried with the pinned toolchain. These commands do not erase or write
flash:

```powershell
$toolRoot = 'C:\Users\developer\tmp\easystick-p4-tools'
& "$toolRoot\Scripts\python.exe" -m esptool --chip esp32p4 --port COM10 `
  --baud 115200 --before default_reset --after no_reset chip_id
& "$toolRoot\Scripts\python.exe" -m esptool --chip esp32p4 --port COM10 `
  --baud 115200 --before default_reset --after no_reset flash_id
& "$toolRoot\Scripts\python.exe" -m esptool --chip esp32p4 --port COM10 `
  --baud 115200 --before default_reset --after hard_reset run
```

The first two commands completed the ROM/stub handshake and reported P4
revision `v1.3`, a 40 MHz crystal, and a detected 16 MiB flash (`manufacturer
0x46`, `device 0x4018`). The final command returned the target from the
bootloader to the stock application without a flash write. This proves the
default-reset read-only probe path; it does not yet prove operator-controlled
GPIO35/CHIP_EN entry or the ten-cycle recovery acceptance test.

Ten consecutive `chip_id` probes using `--after hard_reset` also completed
successfully (`read_only_default_reset_probes=10/10`). These are read-only
reset probes, not the required flash/write/restore recovery cycles.

## What remains unverified

- A read-only `esptool chip-id` request with reset disabled still times out
  when the application is running. The default-reset probe above succeeds,
  but GPIO35/CHIP_EN bootloader entry still needs an operator-controlled
  capture and repeated recovery test.
- A complete 16 MiB stock readback was captured twice outside the repository
  with `esptool.py v4.8.1`; both files are 16,777,216 bytes and have SHA-256
  `229459f251eaf6222f0c07968702d72a3818e520da1788ce465027d969020c24`.
  No restore write or flash/reset recovery cycle has been performed.
- The attached Stamp AddOn C6 revision, C6 flash contents, and SDIO handshake
  have not been identified.
- The stock map above is only a baseline. The Linux partition layout remains
  `TBD_after_M0_measurement` in the board contract until the boot shim, kernel,
  DTB, rootfs, and recovery regions are sized together.

## Readback tool note

`esptool v5.3.1` can enter the ROM/stub path but loses the USB-Serial/JTAG
response around `0x001AB000`, even when the read is reduced to 4 KiB. The
pinned `esptool.py v4.8.1` path reads that sector and completed both full
backups. Keep v4.8.1 for this target's recovery procedure until the newer
transport behavior is explained; do not erase or write the sector as a test.
