# M5Stack Tab5 Linux (ESP32-P4 + ESP32-C6, dual-core SMP)

[![License](https://img.shields.io/badge/license-GPL--2.0--only-blue.svg)](LICENSE)
[![Platform](https://img.shields.io/badge/platform-M5Stack%20Tab5-orange.svg)](https://shop.m5stack.com)
[![CPU](https://img.shields.io/badge/SMP-Dual--Core%20RV32IMAC-green.svg)](https://github.com/tsuyoshi-otake/m5stack-tab5-linux/releases)

Native RISC-V **NOMMU** Linux 7.2 (SMP, both cores of the ESP32-P4) for the **M5Stack Tab5**. It boots to a shell on the USB-C serial console, brings up Wi-Fi through the Tab5's on-board ESP32-C6 (ESP-Hosted-NG over SDIO), starts a SoftAP with DHCP and a Dropbear SSH server, and runs BusyBox, `vi` and MicroPython. The 720x1280 MIPI-DSI panel shows the text console; touch is exposed as an input device.

This repository is the Tab5 port of [m5stamp-p4-c6-linux](https://github.com/tsuyoshi-otake/m5stamp-p4-c6-linux). The kernel patches, boot shim, root filesystem and build scripts are shared with it; what is Tab5-specific is the board bring-up in the boot shim, the device tree, the display and touch drivers (`linux/tab5/`) and the release packaging. Releases here are for the Tab5 only.

> **Status: early preview (0.x).** It was tested on one Tab5 (ST7121 panel). Read [Status](#status) before you flash it, and do not skip the [C6 firmware](#2-c6-firmware-one-time-change) step: it replaces the C6's factory firmware.

---

## Status

Checked on a Tab5 (ESP32-P4 rev v1.3, ST7121 panel). The checks for the v0.6.0 release are in [`linux/tests/v0.6.0-hardware-validation-2026-11-15.md`](linux/tests/v0.6.0-hardware-validation-2026-11-15.md); the rows added after it were checked on `main` on 2026-11-16. Hardware that is not supported yet is listed in #38. Each row names the issue where the result is recorded:

| Area | Result |
| :--- | :--- |
| Boot | Linux 7.2.11 SMP boots to the shell on both cores (`/sys/devices/system/cpu/online` = `0-1`) on the USB-C serial console (#1, #2). Two real power cycles and three reboots in a row came up with no SDIO errors (#22). |
| Wi-Fi hardware | The C6 runs ESP-Hosted-NG 1.0.6 (`NG-1.0.6.0.1`); the kernel logs `mmc0: new SDIO card` and `wlan0` appears (#1). |
| SoftAP + DHCP | `iw dev wlan0 info` reports `type AP`. A Linux station (a Stamp-P4) joined with WPA2, got a lease with gateway and DNS, and pinged the board and back (#4). An Android 15 phone joined with the SoftAP's name and key and had an address, gateway and DNS within about 4 s (#4, #9). The boot script reports `SoftAP started` only when `iw` shows the AP and its SSID with no new SDIO errors; otherwise it re-initialises the C6 once and then reports `SoftAP FAILED`, which `check-boot-log.py` treats as a failure (#22). |
| Wi-Fi QR code | On the first boot the panel shows a QR code for the SoftAP for fifteen minutes. A camera read it back as the board's SSID and key; it clears itself, and it does not come back on the next boot (#9). |
| SSH over Wi-Fi | `m5` logged in with its password through the SoftAP, relayed by the phone above (#4). |
| Hostname | A fresh boot drive names the board `tab5` (#12). |
| Display | Text console on the panel through fbcon (`TER16x32`, 45x40 characters), confirmed by eye (#2). Kernel messages at boot are not shown (`loglevel=4`). A program can also draw through an `mmap` of `/dev/fb0`: four colour bands painted that way by `linux/tests/fb-mmap-test.c` showed on the panel and read back intact (#15). |
| Backlight | `/sys/class/backlight/backlight/brightness` (0 to 1024, LEDC PWM on GPIO22). 0 turns the panel dark and 1024 is full brightness; the change was measured with a camera (#32). |
| Touch | The panel's touch controller (I2C 0x55, up to 10 points) appears as `/dev/input/event0` with the panel's own 720x1280 portrait axes, the same as the stock BSP (no swap or mirror, #33). These are `/dev/fb0` pixel coordinates; the text console is drawn turned 90 degrees (`fbcon=rotate:3`), so a program that works in the console's orientation has to rotate them. `touch-dots` (m3-lab image) draws a dot where the panel is touched; taps on five targets landed within 15 to 27 px. |
| Keyboard | The Tab5 Keyboard (A164, I2C 0x6d) types on the panel console. Letters, `Aa` tapped once and held, the `sym` layer and two keys held together were typed on the board and read back from `/dev/input`; the left LED was checked by eye (#14). `Aa` tapped twice locks upper case and lights the left LED, one more tap unlocks it and the LED goes dark (checked on the LED registers and with a camera). Unplugged and plugged back in while Linux runs, the keyboard is detached and attached again within a second (#13). |
| I/O expanders | The two PI4IOE5V6408 (I2C 0x43, 0x44) are `gpiochip512` and `gpiochip520` with named lines (`SPK_EN`, `EXT5V_EN`, `USB5V_EN`, `PWROFF`, ...). The driver writes nothing at probe, so the outputs the boot shim set stay as they are (#31). |
| Power off | `poweroff` drives the expander's `PWROFF` line and the board turns off; done twice (#31). |
| Power monitor | INA226 (I2C 0x41, 5 mOhm shunt) in `/sys/class/hwmon`: about 5.6 V on USB power (#27). |
| IMU | BMI270 (I2C 0x68) is `iio:device0`; flat on a desk it reads about 1 g on Z (#26). |
| RTC | RX8130CE at I2C **0x32** is `/dev/rtc0`. The clock is restored from it at boot and written back after NTP. After 11 minutes with the power off it was within one second of the PC's clock (#28). |
| USB-A host | The HS USB 2.0 controller (DWC2) runs as a host on the USB-A port: a USB flash drive enumerated as `/dev/sda` and read back (#30). |
| microSD | The slot works over SPI (`spi-gpio` + `mmc_spi`): a 32 GB SDHC card showed up as `mmcblk0` with its partition and read back. FAT and ext2 mount; NTFS does not. It is slow, about 30 KB/s (#29). |
| Bluetooth | The C6 advertises BLE over SDIO and the kernel registers `hci0` (#34). |
| MicroPython | `python3` runs (v1.22.2). |
| Memory | The NOMMU user pool grew by 8 KB over a 32 minute soak. A leak in `bootsyncd` that exhausted the pool within hours on this board is fixed (#3). |

Not verified, or not there yet:

- **Reaching the board from a phone that also has mobile data.** The SoftAP has no internet, so Android keeps mobile data as its default network: on the phone above, a plain `ping` or connection to 192.168.4.1 went out over LTE and failed, while `ping -I wlan0` reached the board. An SSH app on such a phone will likely fail the same way; turn mobile data off while you use the board. The phone joined from its shell (`cmd wifi connect-network`), not by scanning the QR code with its own camera.
- **iPhone, and joining from the QR code with a phone camera**, have not been tried.
- **Station mode** (joining your own Wi-Fi) has not been tried on the Tab5.
- **Other panel lots.** Tab5 units ship with an ST7121 or an ST7123 display, and the oldest ones with an ILI9881C + GT911. The boot shim tells them apart by the touch controller (ST712x at I2C 0x55, GT911 ID at 0x5d) and carries an init path for all three (#36); the GT911 node is enabled only when the shim finds an ILI9881C. Only the ST7121 path has run on real units.
- **Audio** (#24). The I2S0 ASoC driver and the ES8388 codec register a sound card (`Tab5ES8388`, `/dev/snd/pcmC0D0p`), and `pcm-sine` (a minimal player; `aplay` cannot be built for NOMMU) gets through `hw_params`, but playback stops after about 13 ms and no tone comes out of the speaker yet. Headphone output, headphone detection and the microphones (ES7210 has no upstream driver) are not done.
- **Bluetooth from user space.** There is no BlueZ userland on this NOMMU system (bluez5_utils needs MMU), so `hci0` is only reachable through raw HCI sockets; nothing is verified on air (#34).
- **Battery.** Voltage and current come from the INA226; there is no fuel gauge, so no percentage, and the charge-status lines are not exposed as a `power_supply` yet (#31).
- **EXT5V and the external antenna.** The `EXT5V_EN` and `EXT_ANT_EN` expander lines can be set from Linux, but the 5 V output has not been measured and there is no `config.txt` setting yet (#35).
- **Not ported:** the camera (#25: this Linux tree has no V4L2/media support and no MIPI-CSI-2, ISP or sensor driver for the P4, so it is a media-stack port, not a wiring change) and USB device mode (the USB-C port is the serial console).
- **Deep Sleep.** `deepsleep 10` slept and woke on the timer; the board comes back through a normal boot (the P4 wakes by reset) with the panel, touch, keyboard and SoftAP back (#37). Sleep current and waking with the BOOT button have not been checked.
- **No USB drive for the configuration.** The Stamp-P4 build keeps its configuration on a USB mass-storage drive; the Tab5 has no such gadget port, so `credentials.txt`, `config.txt` and autorun are not reachable from a PC. They are the same files with the same keys, in the boot FAT image in flash, and are changed on the board with `boot-edit` ([6. Change the configuration](#6-change-the-configuration)). On a Tab5, `boot-edit --put` added a file and the kernel wrote it to flash. After a hardware reset the boot shim started from that copy, and `boot-edit --rm` removed the file the same way. Editing with `vi`, and losing power during a flash write, have not been tried.

Things that are different from the Stamp-P4 image:

| | Stamp-P4 + AddOn C6 | Tab5 |
| :--- | :--- | :--- |
| Console | USB-C serial | USB-C serial, and the panel (fbcon) |
| Credentials | `credentials.txt` on the USB drive, serial log | serial log (`ES_CREDENTIALS NEW:` once) and `/tmp/credentials.txt` (root) |
| C6 firmware | ESP-Hosted-NG | ESP-Hosted-NG; the Tab5 ships with ESP-Hosted-MCU, so it must be switched once |
| Flash map | shared | the same (A/B `/boot` slots, crash log) |
| Release files | `m5stamp-p4-c6-linux-smp-*` | `m5stack-tab5-linux-smp-*`, with `esp32p4-tab5.dtb` |

---

## Quick Start

### 1. Back up the stock firmware

The release flashes the P4 only, but you need a copy of what is on it to go back. Connect the Tab5's USB-C port to your PC (the port shows up as a serial device, for example `COM13` on Windows or `/dev/ttyACM0` on Linux) and read the whole 16 MiB flash twice:

```bash
esptool.py --chip esp32p4 --port COM13 --baud 460800 read_flash 0 0x1000000 tab5-stock.bin
esptool.py --chip esp32p4 --port COM13 --baud 460800 read_flash 0 0x1000000 tab5-stock-2.bin
sha256sum tab5-stock.bin tab5-stock-2.bin     # both must match
```

M5Stack's firmware is not redistributed here; keep the copy. The flash tool pins esptool 4.8.1.

If esptool cannot connect, put the Tab5 into download mode as M5Stack's Tab5 documentation describes and try again.

Going back to the stock firmware later is `esptool.py --chip esp32p4 --port COM13 write_flash 0x0 tab5-stock.bin`. That restores the P4 only; see the C6 note below.

### 2. C6 firmware (one-time change)

Linux expects the ESP32-C6 to run ESP-Hosted **NG** 1.0.6 (the driver logs `ESP-Hosted Version: NG-1.0.6.0.1`). A Tab5 ships with ESP-Hosted-MCU on the C6, which this driver does not speak, so without this step Linux boots but has no `wlan0`. A mismatch is logged as `Incompatible ESP Host-firmware release detected`; no ESP-Hosted line at all means the C6 is not running NG.

> **This replaces the C6's factory firmware.** The C6 has no USB port of its own. Getting the original C6 firmware back would need the C6's ISP pads and a USB-UART adapter; this project has not tried that, and the stock P4 firmware's Wi-Fi will most likely not work with the NG firmware. Do this only if you accept that.

The release does not carry C6 binaries. The path used during bring-up (#1) updates the C6 over the SDIO link from a temporary P4 image:

1. Build the NG firmware: `bash linux/m2/build-c6.sh --vendor <out>` (`C6_BT=0` leaves out the BLE controller; the default `C6_BT=1` keeps it and the kernel registers `hci0` for it, see [Status](#status)). `<out>/C6_PATCH_MANIFEST.txt` records the `network_adapter.bin` SHA-256 and the patches.
2. Build the temporary updater for the Tab5 wiring: `ES_BOARD=tab5 bash linux/m2/build-ota-host.sh <external-source-root> <out>/build/network_adapter.bin <ota-out>`. `<external-source-root>` holds `esp_idf` and an `esp_hosted_mcu` checkout (v2.13.0 or newer). The Tab5 variant switches the C6 supply on through the I/O expander before ESP-Hosted resets the C6.
3. Write it with `linux/m2/ota-host/flash-ota-host.ps1 -Port COM13 -PythonPath python -ArtifactsDirectory <ota-out> -StockReadback tab5-stock.bin -AllowCandidateWrite`. It refuses to run without a stock readback of this board.
4. The updater sends the C6 image over SDIO on its next boot. Then write the Linux release image as in step 3 below.

### 3. Flash Linux

Download the release package from [Releases](https://github.com/tsuyoshi-otake/m5stack-tab5-linux/releases):

- **`m5stack-tab5-linux-smp-v0.1.0.zip`**: the partition binaries and `flash-candidate.ps1`.
- **`m5stack-tab5-linux-smp-v0.1.0-16mb.bin.gz`**: the same as one 16 MiB image.

#### Option A: PowerShell script (recommended)

Unzip the package and run, from the folder:

```powershell
.\flash-candidate.ps1 -Port COM13 -AllowCandidateWrite -StockReadback ..\tab5-stock.bin
```

Write with this script rather than calling esptool directly: it prints each artifact's SHA-256 and the kernel build it wrote (`Expect after boot: uname -v = …`), so the booted image can be reconciled against what was written. It checks the artifact sizes, writes the bootloader, partition table, boot shim, kernel, root filesystem and DTB, verifies them, and resets the board so Linux boots (pass `-NoReset` to leave it in download mode instead). The boot slots (`/boot` with your Wi-Fi settings and credentials) and the A/B metadata are kept by default; `-WriteBoot` writes `boot.img` and resets the A/B choice (needed the first time). It also checks the board twice: the artifact DTB's model must match `-Board` (default: the package's own board — Tab5 here), and it reads the DTB already in flash so writing a Tab5 image over a board that last ran Stamp-P4 stops unless you pass `-AllowBoardChange`. `-StockReadback` is optional: it makes the script check that the file is a 16 MiB dump with the SHA-256 `a260eda9…` of the author's unit, so pass `-ExpectedStockSha256 <hash>` with the hash of your own dump, or leave it out.

The first install needs `-WriteBoot` so that the boot FAT image exists:

```powershell
.\flash-candidate.ps1 -Port COM13 -AllowCandidateWrite -WriteBoot
```

#### Option B: manual esptool

```bash
esptool.py --chip esp32p4 --port COM13 --baud 460800 \
    --before default_reset --after no_reset write_flash -z \
    0x002000 bootloader.bin \
    0x008000 partition-table.bin \
    0x010000 boot-shim.bin \
    0x090000 Image \
    0x810000 rootfs.squashfs \
    0xf10000 esp32p4-tab5.dtb \
    0xf40000 boot.img
esptool.py --chip esp32p4 --port COM13 --baud 460800 \
    --before default_reset --after hard_reset erase_region 0xfc0000 0x2000
```

#### Option C: one 16 MiB image

```bash
gzip -d m5stack-tab5-linux-smp-v0.1.0-16mb.bin.gz
esptool.py --chip esp32p4 --port COM13 --baud 460800 write_flash 0x0 m5stack-tab5-linux-smp-v0.1.0-16mb.bin
```

This overwrites the whole flash, including the stock NVS and PHY data and any credentials from an earlier install.

#### Flash map

| Offset | File | Description |
| :--- | :--- | :--- |
| `0x002000` | `bootloader.bin` | ESP-IDF first stage bootloader |
| `0x008000` | `partition-table.bin` | Partition table |
| `0x010000` | `boot-shim.bin` | Second stage (PSRAM, SDIO, display, C6 reset, CPU1 release, jump to Linux) |
| `0x090000` | `Image` | Linux 7.2 SMP RISC-V kernel |
| `0x810000` | `rootfs.squashfs` | Read-only SquashFS root filesystem (Buildroot userland) |
| `0xf10000` | `esp32p4-tab5.dtb` | Device tree for the Tab5 |
| `0xf20000` | — | Crash log |
| `0xf40000` | `boot.img` | 256 KB FAT12 `/boot` slot 0 (Wi-Fi settings, credentials) |
| `0xf80000` | — | Second `/boot` slot, for power-loss-safe updates |
| `0xfc0000` | — | Two 4 KB A/B metadata sectors |

### 4. First boot

The Tab5 starts Linux on its own after flashing. Open the serial port at 115200 8N1 and press Enter for a root shell; the panel shows the same console.

On the first boot the board makes its own credentials. They are printed once on the serial console as `ES_CREDENTIALS NEW: …` and stay readable as root in `/tmp/credentials.txt`:

```text
cat /tmp/credentials.txt
```

`ssid=` (`m5-XXXX`) and `psk=` are the SoftAP, `password=` is the SSH and console password of user `m5`, and `root_password=` is what `su` asks for. **The serial log then contains the passwords; redact that line before you share logs.** There is no shared default: every board gets its own.

On that first boot only, the panel shows the SoftAP credentials as a QR code for fifteen minutes — point a phone camera at it to join `m5-XXXX`. To show it again later, run as root: `wifi-qr --panel --for 60` (or `wifi-qr > /dev/tty0` for a text QR); `wifi-qr --clear` erases it.

### 5. Connect over Wi-Fi

1. Join the Wi-Fi network `m5-XXXX` (WPA2-PSK, 2.4 GHz) with the `psk=` from `credentials.txt`.
2. The board's DHCP server hands out `192.168.4.2` – `192.168.4.20` with the gateway and DNS `192.168.4.1`.
3. Log in:
   ```bash
   ssh m5@192.168.4.1       # the password= from credentials.txt
   ```
4. Edit and run programs: `vi hello.py`, `python3 hello.py`.

| Service | Address | User | Password |
| :--- | :--- | :--- | :--- |
| Wi-Fi SoftAP | `m5-XXXX` | — | `psk=` |
| SSH (Dropbear) | `192.168.4.1:22` | `m5` | `password=` |
| Serial console | USB-C, 115200 8N1 | root shell | none |

Root over SSH is key-only; on the Stamp that needs a file on the USB drive, which the Tab5 does not have.

For everything the userland shares with the Stamp-P4 build (the shell, `vi`, MicroPython, joining another Wi-Fi network, the `/boot` configuration files, Deep Sleep), see the [Stamp-P4 README](https://github.com/tsuyoshi-otake/m5stamp-p4-c6-linux#readme). Its USB-drive instructions do not apply here, and none of it has been re-tested on the Tab5 except where [Status](#status) says so. Serial access, troubleshooting and recovery tips are in [docs/troubleshooting.md](docs/troubleshooting.md).

### 6. Change the configuration

The Stamp-P4 README tells you to open the USB drive and edit `config.txt`, `credentials.txt`, `wpa_supplicant.conf`, `authorized_keys` or the autorun program there. The Tab5 has no USB drive, so do the same on the board, as root, with `boot-edit`:

```sh
boot-edit --ls                      # what is on the drive
boot-edit config.txt                # edit with vi; saved when you leave vi
boot-edit --cat credentials.txt     # print a file
boot-edit --put authorized_keys < /tmp/id_ed25519.pub
boot-edit --rm main.py
```

Everything else is as on the Stamp-P4: a change is checked, written to flash and applied about three seconds later, and the console shows the same `[bootsync] Saved to flash` line. `boot-edit` waits for that line's result and says `saved to flash`, or tells you that the save was not confirmed.

### 7. Keyboard (optional)

The M5Stack Tab5 Keyboard (A164) types on the panel console. Plug it in before or after power-on; the kernel logs `tab5-keyboard: attached` within a second, and unplugging it releases every key it was holding.

The keys work as they do with M5Stack's own firmware:

| Key | What it does |
| :--- | :--- |
| `sym`, held | The symbol keys type their second character (`!` becomes `?`, `(` becomes `<`, `.` becomes `,`). |
| `Aa`, one tap | The next letter is upper case. The left LED blinks until you type it. |
| `Aa`, two taps | Letters stay upper case until you tap `Aa` again. The left LED stays lit. |
| `Aa`, held | A Shift key for letters. |
| `ctrl`, `alt` | As on a PC keyboard: `ctrl` + `c` stops a program. |

The right LED, which the keyboard lights all the time on its own, is kept dark. The left LED's brightness is `m5stack,led-brightness` (0–100, 0 for off) in `linux/dts/esp32p4-tab5.dts`.

---

## Security Design

The release images are set up as a development board. The security model (#60–#62 of the Stamp-P4 project) is:

- **Two boundaries.** Over the network everything needs authentication: Wi-Fi is WPA2-PSK and SSH needs the per-device random password or a key. Over USB, whoever can plug in has the owner's rights: the serial console is a root shell and esptool can rewrite the flash. Keeping the board in your hands is the boundary.
- **Code running on the board owns the whole device.** User programs run in RISC-V machine mode (M-mode), like the kernel, so any program can write kernel memory, CSRs, peripherals and the flash. The `m5`/root split and `su` keep mistakes apart; they do not stop an attacker.
- **Why user programs do not run in U-mode.** Without an MMU every process shares one memory pool, so a compromised `m5` program can rewrite a root daemon (Dropbear, inetd) and become root. ESP-IDF's startup code in the boot shim also locks 14 of the 16 PMP entries, and the locked entries let U-mode read and write the peripherals, including the SPI flash controller. See the Stamp-P4 project's #61 for the measurements.
- **Secure Boot and flash encryption are disabled.** They burn eFuses, which cannot be undone, and they would remove the recovery path. Physical access allows reading the flash (including the Wi-Fi credentials in the boot FAT image) and writing arbitrary firmware. [docs/security-hardening.md](docs/security-hardening.md) lists what a product build needs, and [SECURITY.md](SECURITY.md) lists the known limitations.
- **Wi-Fi regulatory domain**: no `country=` is set by default, so the world regulatory domain applies. WPA2-PSK only; WPA3-SAE is not built.

---

## Repository Structure

```text
.
├── board/                 # Stamp-P4 carrier schematics (inherited; not used by the Tab5)
├── docs/                  # Architecture, troubleshooting, and post-mortem deep dives
├── linux/                 # Linux kernel, Buildroot, boot-shim, and integration
│   ├── boot-shim/         # ESP-IDF second stage (tab5_board.c / tab5_display.c are Tab5-only)
│   ├── buildroot-external/# Buildroot packages, rootfs overlays (board/tab5)
│   ├── dts/               # Device trees (esp32p4-tab5.dts)
│   ├── kernel-patches/    # Linux patches shared with the Stamp-P4 build
│   ├── tab5/              # Tab5 kernel patches (display scan-out, touch, GPIO) and flash-layout.json
│   ├── m2/                # C6 firmware build and the temporary OTA host
│   ├── m3-lab/            # Lab profile overlays (SoftAP, udhcpd, Dropbear, vi)
│   ├── tests/             # Hardware validation records
│   ├── build-c68.sh       # End-to-end containerized build (ES_BOARD=tab5)
│   └── flash-candidate.ps1# Flash script for esptool
├── tools/                 # Environment validation, secret check, source lock
├── vendor/                # Pinned upstream Git submodules
├── versions.lock.json     # Pinned commits and toolchain hashes
└── board-contract.json    # Pin assignments and electrical contract (Tab5 hardware sources)
```

---

## Building from Source

The build is the one CI runs for a release. It needs a Linux host with the packages that `.github/workflows/release.yml` installs, and ESP-IDF from `vendor/esp-idf`.

```bash
git clone --recursive https://github.com/tsuyoshi-otake/m5stack-tab5-linux.git
cd m5stack-tab5-linux
(cd vendor/esp-idf && ./install.sh esp32p4)
export IDF_PATH=$PWD/vendor/esp-idf
. "$IDF_PATH/export.sh"

ES_BOARD=tab5 bash linux/release-build.sh --vendor /path/to/out --profile m3-lab
```

`release-build.sh` is the same wrapper the release workflow calls; it sets the three `NOMMU_*` variables that are the release settings of the user-pool allocator, then runs `linux/build-c68.sh`. This compiles the boot shim, the Linux 7.2 SMP kernel with the Tab5 device tree and patches, and the Buildroot root filesystem with BusyBox, Dropbear, MicroPython and the ESP-Hosted-NG driver. Without `ES_BOARD=tab5` the build is for the Stamp-P4.

`linux/tools/package-release.py --board tab5 --build-output /path/to/out --boot-image /path/to/out/buildroot/images/boot.img --output release --version v0.1.0` makes the release files.

---

## SHA-256 Checksums

Each release lists its checksums in `SHA256SUMS.txt`. v0.1.0 was validated on hardware from exactly these files; the record is in [`linux/tests/`](linux/tests/).

---

## Third-party licenses

The images contain third-party software under its own licenses. Corresponding source for GPL components is built from the revisions pinned in `versions.lock.json` plus the patches and configs under `linux/`.

| Component | License |
|---|---|
| Linux 7.2.11 + patches, `esp32_sdio.ko` | GPL-2.0 |
| BusyBox | GPL-2.0 |
| uClibc-ng | LGPL-2.1 |
| Dropbear | MIT |
| wpa_supplicant | BSD-3-Clause |
| ESP-IDF (boot-shim) | Apache-2.0 |
| ESP-Hosted NG host driver (`esp32_sdio.ko`) | GPL-2.0-only (shared headers GPL-2.0-only or Apache-2.0) |
| ESP-Hosted NG C6 firmware (`network_adapter.bin`) | Apache-2.0 |

Releases carry `…-legal-info.tar.gz`, Buildroot's `make legal-info` output for the image: the source archives, patches, configs and license texts of every package in the rootfs and the kernel (`manifest.csv` lists them). Sources of build-host tools (`host-sources`) are left out because they are not shipped. Build it locally with `LEGAL_INFO=1 bash linux/build-c68.sh …`; it lands in `<vendor>/buildroot/legal-info`. The boot shim and the C6 firmware are built from the ESP-IDF and ESP-Hosted revisions in `versions.lock.json`. The image itself lists the same pointers in `/usr/share/licenses/README`.

The project's own files are licensed under GPL-2.0-only ([`LICENSE`](LICENSE)), except where a file carries its own `SPDX-License-Identifier`: the boot shim (`linux/boot-shim/`) is `Apache-2.0 OR GPL-2.0-only`, and the schematics and Gerbers under `board/hardware/` are MIT ([`board/hardware/LICENSE`](board/hardware/LICENSE)).

## Acknowledgments & References

- [m5stamp-p4-c6-linux](https://github.com/tsuyoshi-otake/m5stamp-p4-c6-linux), the Stamp-P4 project this is a port of.
- [why2025-linux](https://github.com/mrbreaker/why2025-linux) by @mrbreaker for initial ESP32-P4 NOMMU bring-up reference.
- [Espressif ESP-Hosted](https://github.com/espressif/esp-hosted) for the SDIO Wi-Fi slave stack.
- The Buildroot and Linux kernel communities.
