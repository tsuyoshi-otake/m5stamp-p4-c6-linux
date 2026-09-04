# M3-lab SDIO aggregate checkpoint — 2026-08-10

Status: **P4 flash and Linux boot passed; this diagnostic image did not
produce `wlan0` or SSH**.  This is a follow-up to the earlier reset-before-
enumeration run that reached the ESP-Hosted boot event.  It records the
aggregate receive-queue patch and its result without changing the C6 image.

## Build

- Profile: `build-m1.sh --profile m3-lab`
- Host build: Docker `easystick-p4-build:latest`
- Persistent compiler cache: Docker volume `easystick-p4-ccache`
- Added diagnostic: `package/esp-hosted-ng/0009-easystick-sdio-rx-aggregate.patch`
- Candidate output: external directory
  `C:\Users\developer\tmp\easystick-p4-m3-sdioagg-20260810`

| Artifact | Size | SHA-256 |
| --- | ---: | --- |
| `Image` | 6,497,284 bytes | `bc1ba145e7346616f1df321dedb75bbf8417cc22b8cbbba82e7dd054af541182` |
| `rootfs.squashfs` | 1,826,816 bytes | `9f96804f0d6adb5817c03d1cb873bf72f39897752ad0135ac4506f66e628d560` |
| `easystick-stamp-p4.dtb` | 2,858 bytes | `a4284b333bfee614c6f41e7ed8566a910af7eca9916cff750a06c62fc439b0f7` |

`verify-images.py` passed the candidate overlap and 16 MiB bounds checks.

## Flash and boot

The P4-only write used `flash-candidate.ps1` with the preserved stock
readback as its recovery guard.  esptool 4.8.1 verified every P4 artifact;
the C6 was not reflashed.  The raw capture is outside Git:

```text
C:\Users\developer\tmp\easystick-p4-m3-sdioagg-boot-20260810.log
SHA-256 62c17f7d8f5568e4615b9a4cf2826b5bf1791fd5ba4a750c0e3839174c0310c0
```

The capture shows Linux 6.18.35, SquashFS/init, SDIO card discovery, and the
ESP-Hosted probe.  It also shows:

```text
Rx Pos ====== 0
Tx Pos ====== 0
OPEN_DATA_PATH write completed: ret=0
SDIO IRQ received ... status=0x00000000
read_packet: BOOT_CMD53_RX: len=0 ret=0
M3-lab: no wlan0; SSH remains disabled
```

There is no accepted boot-up event, ESP-Hosted version report, `wlan0`, DHCP,
or SSH listener in this run.  The earlier reset-before-enumeration capture
remains the functional baseline; this aggregate experiment is therefore
diagnostic only and must not be described as a regression fix.

## Next action

Keep the known-good C6 image unchanged.  Investigate the P4-side reset/
enumeration and pending-frame lifecycle, then capture CMD52/CMD53 status and
SDIO DAT1/controller interrupt state.  Do not add network credentials or
declare SSH acceptance until the boot frame is decoded and Wi-Fi association,
DHCP, and a key-authenticated session are captured.
