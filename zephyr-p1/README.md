# EasyStick Stamp-P4 Zephyr P1 control image

Status: **WIP — P1-NG negative-control image; not a manufacturing image**.

## Current test condition — 2026-08-24

The target is the M5Stack Stamp-P4 host with its attached Stamp-AddOn C6.
The attached C6 remains unchanged on the existing ESP-Hosted-NG 1.0.6 image;
no C6 backup, erase, or write is part of this run. The Zephyr host may be
flashed to the P4 only to measure the failure boundary against that known
incompatible companion.

This condition cannot produce full P1 acceptance. A successful result here is
named `P1-NG negative-control observed`, not `P1 PASS`. In particular, it
cannot establish Wi-Fi scan, association, DHCPv4, or ping.

This application uses the pinned Zephyr Function-EV HP-core board support as
its base and applies `boards/esp32p4_function_ev_board_esp32p4_hpcore.overlay`
for the EasyStick carrier wiring. It does not copy or fork the Function-EV
board definition.

The image enables only the P1 path:

```text
C6 reset → SDIO enumerate → Function 1 → CMD52 → small CMD53
→ 512-byte CMD53 → repeated CMD53 → ESP-Hosted init
→ Wi-Fi scan → association → DHCPv4 → ICMPv4 ping
```

SSH, NTP, SMP, and Bluetooth are outside this gate. Bluetooth is disabled in
the overlay.

## Pinned inputs

| Input | Revision |
| --- | --- |
| Zephyr | `d544481d9ad9c711cefe984c5ea926d71cb56341` |
| ESP-Hosted-MCU slave | `3f0d1076749afdb589f00c075d8dce895e3dd32d` |
| Expected C6 firmware | `2.12.12` |

The host transport ladder and strict firmware-version check are carried by a
small local patch in `patches/`. The generated image and build directory must
remain outside Git.

## Reproducible build and preflight

The C6 image is built by:

```powershell
powershell -NoProfile -ExecutionPolicy Bypass -File `
  .\projects\easystick-stamp-p4\firmware\tools\build-p1-c6.ps1 `
  -BuildDirectory D:\Users\Developer\easystick-tmp-20260820\build-p1-c6-rNN
```

The script checks the C6, ESP-IDF, and protobuf-c revisions before invoking
the `espressif/idf:v5.5.3` container. Its container includes the pinned
ESP-IDF revision; the host checkout is checked independently. Use a new empty
external build directory after a failed attempt.

The Zephyr host image is built with:

```powershell
powershell -NoProfile -ExecutionPolicy Bypass -File `
  .\projects\easystick-stamp-p4\firmware\tools\build-p1-zephyr.ps1 `
  -BuildDirectory D:\Users\Developer\easystick-tmp-20260820\build-p1-zephyr-rNN
```

The default host container is
`zephyrprojectrtos/ci@sha256:e3d3643e50dbbbb22aa6e3efd65d9edc8a281d3e113e03c39527a896b1feedc0`
(pulled as `v0.29.3-amd64`). It contains the Zephyr SDK 1.0.1 and CI
toolchains without the larger developer/VNC layer. The build script installs
the pinned Zephyr source's `scripts/requirements-base.txt` at container
runtime so the Python dependencies used by Zephyr 4.4 are available. A
different container may be supplied with `-ContainerImage` after confirming
that it contains the Zephyr SDK. Once `west update` has completed, a retry may
use `-SkipWestUpdate` with the same external workspace to avoid refetching all
manifest repositories. A failed or interrupted build may use
`-ReuseBuildDirectory` to let Ninja/CMake retain successful object files;
without that switch, the script requires an empty build directory and uses a
pristine build.

Run `verify-p1-build.ps1` only after both images exist. It is a source/DT
preflight gate, not proof of hardware behaviour.

## Runtime controls

The USB Serial/JTAG console provides:

```text
p1 scan
p1 connect "<ssid>" "<psk>"
p1 ping <ipv4-address>
p1 status
```

Credentials are supplied at runtime and are not stored in `prj.conf` or the
repository. `p1 ping` requires both the association marker and a DHCPv4
address.

## Hardware evidence

For the current negative-control run, do not use the C6 readback or write
scripts. Run `flash-p1-zephyr.ps1` with the preserved P4 stock readback,
`-AllowP1NgWrite`, and the verified Zephyr image hash. The script writes only
the P4 application at the generated runner address and performs a readback
verification; it has no C6 operation. `COM10` is the P4 host port.

After the P4-only write gate passes, `capture-p1-uart.py` records raw P4
console bytes without transmitting commands. For this condition, capture the
startup boundary and run `verify-p1-ng-capture.py`; its PASS means only that a
contiguous transport prefix and a subsequent failure were observed. The
all-11-marker `verify-p1-capture.py` remains the verifier for a future matching
C6 control run.

## Build contract

Build with the external Zephyr checkout, the explicit overlay, and a clean
build directory. The build record must retain the Zephyr revision, patch
hash, merged DTS, generated `zephyr/.config`, and image SHA-256. Under the
current condition, flash only through the explicit P4-only
`flash-p1-zephyr.ps1` gate. The C6 candidate image is a build/reference
artifact and must not be written.
