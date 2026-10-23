# SDIO clock above 5 MHz (2026-10-16, #21)

Board: M5Stamp ESP32-P4 + C6 on COM10; only the P4 flash was written.

## Image

Local m3-lab candidate from d4495e6 (`sdio_mhz=` in config.txt) built with
`SDIO_MAX_KHZ=40000`, the DTS cap the repository now uses by
default.  Its DTB (`a12b18e1…`) is byte-identical to the v0.2.2 DTB with
only `max-frequency` changed from `<0x4c4b40>` to `<0x2625a00>`.

| File | sha256 |
|---|---|
| `esp32p4-m5stamp-p4.dtb` | `a12b18e1347ade36c499c79338ff7125bd4140bf56c64ecfedce8f93b4d3a852` |
| `rootfs.squashfs` | `7d6014cd2b6f915d89b071b8a3af9bed35e14d3d6ca3ae959a6b5897d50248d3` |
| `Image` | `96e48b33e75e27cd3692fcaf6951cf233981efc36b1fddbe7751b66bc4c813ba` |

`sdio_mhz` was written into the boot FAT's `config.txt` on the board
(loop-mount a copy of `/tmp/boot.img`, edit, `boot-commit`) and
applied with `reboot`: `esp32_sdio` cannot be unloaded, so the rate is
fixed for the boot.

## Clock sequence

With the 40 MHz cap the MMC core runs the C6 at 400 kHz for enumeration,
then at 20 MHz (it asks for 25 MHz, the C6's non-high-speed limit; the
divider gives 40/2), and `esp32_sdio` then applies its own rate:

```
ES_SDIO_CLOCK_HW ... ios_clock_hz=400000 actual_clock_hz=400000
ES_SDIO_CLOCK_HW ... ios_clock_hz=25000000 actual_clock_hz=20000000 divider=1
ES_SDIO_CLOCK_HW ... ios_clock_hz=5000000 actual_clock_hz=5000000 divider=4
ES_SDIO_CLOCK_APPLY requested_mhz=5 ios_clock_hz=5000000 actual_clock_hz=5000000
```

`sdio_mhz=10`, `20` and `40` gave `actual_clock_hz` 10000000, 20000000 and
40000000 (40 MHz is the CIU clock undivided, in default-speed timing).

## Throughput

Station mode on the home access point (the PC's only link is Wi-Fi).  Raw
TCP is BusyBox `nc` on the board and a Python socket on the PC
(`nct.py`); SSH is `cat` of a 1 MiB random file through Dropbear, sha256
checked.  KB/s; each cell lists the runs.

| Clock | Raw TCP PC→board (2 MiB) | Raw TCP board→PC (1–2 MiB) | SSH PC→board | SSH board→PC |
|---|---|---|---|---|
| 5 MHz (v0.2.2 image) | 299, 303, 327 | 307 | 215 | 221 |
| 10 MHz | 377, 354 | 276, 157 | 211 | 209 |
| 20 MHz (two boots) | 386, 407, 384, 407 | 270, 299, 149, 307 | 55, 235, 232, 233, 229, 235, 238, 232 | 226, 226, 181, 218, 230, 142, 214 |
| 40 MHz | 412, 442 | 249, 317 | 254, 249, 246, 246 | 246, 239, 225, 244 |

Board-to-PC results scatter with the Wi-Fi (the low values come and go at
every rate); the single 55 KB/s SSH upload did not repeat.  Raising the
clock from 5 to 40 MHz gains about a third on raw TCP and about 15 % on
SSH.  During a 5 MHz upload the CPU was about 36 % idle, so the bus clock
was not the only limit; for SSH, Dropbear's crypto on the single 360 MHz
core caps the rate.  No SDIO error and no `Allocation of length` line
appeared in dmesg after any run.

## Boot checks

| Run | Result |
|---|---|
| Default (5 MHz requested) after the flash | `check-boot-log.py` 11/11 |
| `sdio_mhz=40`, 10 resets (`capture-boot.ps1 -Reset`) | 11/11 each, no `Allocation of length` |

## Not covered

- A soak of hours at 20 or 40 MHz; 5 MHz stays the default until one runs.
- High-speed timing (`cap-sd-highspeed`): the C6 is clocked at 40 MHz in
  default-speed timing.
- SoftAP-mode throughput.
