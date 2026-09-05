# M5Stamp ESP32-P4 + C6 SDIO Linux (SMP Dual-Core)

[![License](https://img.shields.io/badge/license-GPLv2%20%2F%20MIT%20%2F%20Apache-blue.svg)](LICENSE)
[![Platform](https://img.shields.io/badge/platform-ESP32--P4%20%2B%20ESP32--C6-orange.svg)](https://m5stack.com)
[![CPU](https://img.shields.io/badge/SMP-Dual--Core%20RV32IMAC%20%40%20400MHz-green.svg)](https://github.com/tsuyoshi-otake/m5stamp-p4-c6-linux/releases)

A reproducible, native **Dual-Core SMP** RISC-V NOMMU Linux 6.18 distribution for the **M5Stack Stamp-P4** carrier board paired with **Stamp-AddOn C6** (via 4-bit SDIO interface).

Boots out-of-the-box into **Wi-Fi SoftAP mode** with automatic DHCP, Dropbear SSH server, BusyBox `vi`, MicroPython (`python3`), and a writable OverlayFS on `/home`, `/root`, and `/var/lib`.

---

## Key Features & Verified Hardware Capabilities

- **SoC Target**: ESP32-P4 (Dual-Core RV32IMAC / ilp32 @ 400 MHz, 32 MiB PSRAM, 16 MiB SPI Flash).
- **SMP Enabled**: Both CPU0 and CPU1 active (`Linux 6.18.35 #1 SMP riscv32 GNU/Linux`).
- **Wi-Fi Companion**: ESP32-C6 (M5Stack Stamp-AddOn C6) connected via high-speed 4-bit SDIO.
- **Out-of-the-Box SoftAP**:
  - Automatically broadcasts Wi-Fi SSID: **`m5`**
  - WPA2-PSK Password: **`m5stamp-p4-c6`**
  - Integrated `udhcpd` DHCP server assigning client IPs in `192.168.4.2` – `192.168.4.20` (Gateway: `192.168.4.1`).
- **Pre-installed Developer Tools**:
  - **SSH Server**: Password-enabled Dropbear on TCP port 22 (`m5` / `m5stamp-p4-c6`).
  - **`vi` Editor**: Full-featured BusyBox `vi` with search, undo, visual buffers, and syntax assistance.
  - **MicroPython (`python3`)**: MicroPython v1.22.2 compiled with bFLT Load-to-Ram (`-r`) and position-independent code (`-fPIC`) for stable NOMMU execution.
  - **Writable Filesystem (OverlayFS)**: While the root filesystem is SquashFS (read-only for durability), RAM-backed OverlayFS is automatically mounted on `/home`, `/root`, and `/var/lib` so users can create, edit, and save scripts freely.
- **Kernel & SoC Stabilization**:
  - Signal delivery `mcause` hardening (`0014`).
  - L2 unified cache & DMA reentrancy hardening (`0015`).
  - Systimer 52-bit atomic multi-word read and missed-alarm mitigation (`0016`).
  - Dual-core systimer lock and CPU1 context switch tracking (`0042`, `0044`, `0045`, `0046`).
  - SDIO DW-MMC host controller driver enhancements with software DTO and buffer pacing (`0032`, `0033`, `0060`, `0061`).

---

## Default Credentials & Connection

| Service | SSID / Host | Username | Password | Notes |
| :--- | :--- | :--- | :--- | :--- |
| **Wi-Fi SoftAP** | `m5` | — | `m5stamp-p4-c6` | WPA2-PSK, 2.4 GHz 802.11b/g/n |
| **SSH (`dropbear`)**| `192.168.4.1:22` | `m5` | `m5stamp-p4-c6` | Default user with home directory `/home/m5` |
| **Serial Console** | `COMx` (115200 8N1) | *(root shell)* | *(none)* | `ttyGS1` over USB-Serial/JTAG. Press `Enter` to activate |

---

## Quick Start: Flash Pre-Built Release Binaries

### 1. Download Release Package

Download the latest release archive `m5stamp-p4-c6-linux-smp-v0.1.0.zip` from the [Releases](https://github.com/tsuyoshi-otake/m5stamp-p4-c6-linux/releases) page and extract it.

### 2. Flash using `esptool.py`

Connect the M5Stamp-P4 via its USB-C port to your computer. Identify your serial port (e.g., `COM10` on Windows or `/dev/ttyACM0` on Linux/macOS).

Run the following command:

```bash
esptool.py --chip esp32p4 --port COM10 --baud 460800 \
    --before default_reset --after hard_reset write_flash -z \
    0x002000 bootloader.bin \
    0x008000 partition-table.bin \
    0x010000 boot-shim.bin \
    0x090000 Image \
    0x810000 rootfs.squashfs \
    0x0f10000 easystick-stamp-p4.dtb
```

#### Flash Partition Map

| Offset | File | Description |
| :--- | :--- | :--- |
| `0x002000` | `bootloader.bin` | ESP-IDF 1st stage bootloader |
| `0x008000` | `partition-table.bin` | ESP32-P4 Flash partition table |
| `0x010000` | `boot-shim.bin` | Secondary boot-shim (PSRAM, SDIO matrix, CPU1 release, jump to Linux) |
| `0x090000` | `Image` | Linux 6.18 SMP RISC-V kernel binary |
| `0x810000` | `rootfs.squashfs` | Read-only SquashFS root filesystem with Buildroot userland |
| `0x0f10000` | `easystick-stamp-p4.dtb` | Compiled Flattened Device Tree for Stamp-P4 |

### 3. Connect & Use

1. After flashing, the Stamp-P4 automatically reboots and starts SoftAP within ~10 seconds.
2. On your laptop or smartphone, connect to Wi-Fi SSID **`m5`** using password **`m5stamp-p4-c6`**.
3. Your device will receive an IP address such as `192.168.4.2`.
4. Open a terminal and SSH into the board:
   ```bash
   ssh m5@192.168.4.1
   # Password: m5stamp-p4-c6
   ```
5. You can now edit files and run Python programs immediately:
   ```bash
   vi hello.py
   python3 hello.py
   ```

---

## Hardware Pinout (Stamp-P4 <-> Stamp-AddOn C6)

| Signal | ESP32-P4 Pin | ESP32-C6 Pin | Function |
| :--- | :--- | :--- | :--- |
| **C6_PU / RESET** | GPIO 42 | CHIP_PU | Reset line for C6 slave (controlled by boot-shim) |
| **SDIO_CLK** | GPIO 43 | GPIO 19 | SDIO Clock |
| **SDIO_CMD** | GPIO 44 | GPIO 18 | SDIO Command |
| **SDIO_DAT0** | GPIO 45 | GPIO 21 | SDIO Data 0 |
| **SDIO_DAT1 / IRQ**| GPIO 46 | GPIO 22 | SDIO Data 1 / Interrupt |
| **SDIO_DAT2** | GPIO 47 | GPIO 23 | SDIO Data 2 |
| **SDIO_DAT3** | GPIO 48 | GPIO 15 | SDIO Data 3 |

---

## Technical Deep Dive: The "SSH Wedge" & Kernel Stabilization

Bringing up native Linux on early RISC-V NOMMU silicon often uncovers subtle interaction issues between hardware errata and kernel subsystems.

During initial development, a critical failure known as the **"SSH Wedge"** manifested: executing any non-interactive SSH command (`ssh m5@host id`) or child process termination caused an immediate communication lockup, followed by a hardware watchdog reset (`rst:0x7`) ~120–160s later.

While initially suspected to be an SDIO bus saturation or ESP-Hosted-NG flow-control overflow, extensive post-mortem debugging via RTC crash capsules and local loopback reproduction uncovered an unhandled `Load access fault` in `rb_erase()` during child process exit (`timerqueue_del() -> rb_erase()`).

This was resolved by integrating three essential ESP32-P4 SoC hardening patches:
1. **Signal `mcause` hardening (`0014`)**: Fixes interrupt masking register corruption on `SIGCHLD` returns.
2. **L2 Unified Cache hardening (`0015`)**: Prevents silent memory corruption by eliminating blind L2 cache invalidation without write-back.
3. **Systimer atomic read hardening (`0016`)**: Eliminates torn reads on 52-bit hardware counters and missed timer alarms.

👉 **Read the complete engineering post-mortem and root-cause analysis in [docs/deep-dive-ssh-wedge-and-soc-hardening.md](docs/deep-dive-ssh-wedge-and-soc-hardening.md).**

---

## Repository Structure

```text
.
├── board/                 # Hardware contract and pinout specifications
├── docs/                  # Architecture, troubleshooting, and post-mortem deep dives
│   └── deep-dive-ssh-wedge-and-soc-hardening.md
├── linux/                 # Linux kernel, Buildroot, boot-shim, and integration
│   ├── boot-shim/         # ESP-IDF 2nd stage bootloader (PSRAM/SDIO/flash setup)
│   ├── buildroot-external/# Buildroot package definitions & rootfs overlay
│   │   └── package/
│   │       ├── esp-hosted-ng/     # SDIO Wi-Fi kernel driver with AP mode support
│   │       └── micropython-nommu/ # MicroPython port configured for NOMMU bFLT
│   ├── dts/               # Device Tree Source (Stamp-P4)
│   ├── kernel-patches/    # Target-specific Linux kernel & SMP hardening patches
│   ├── c68/               # Dual-core SMP kernel config fragments & patch renderers
│   ├── m3-lab/            # Lab profile overlays (OverlayFS, SoftAP, udhcpd, vi)
│   ├── build-m1.sh        # Reproducible end-to-end containerized build script
│   ├── flash-candidate.ps1# Flashing script for esptool
│   └── flash-layout.json  # Flash partition map
├── tests/                 # Real hardware validation captures, test scripts, and records
├── tools/                 # Environment validation, UART capture, and flashing utilities
├── vendor/                # Pinned upstream submodules
├── versions.lock.json     # Immutable commit and toolchain hashes
└── board-contract.json    # Pin assignment & electrical contract
```

---

## Building from Source

The build pipeline is fully automated and uses Docker to provide a reproducible Debian build environment:

```bash
git clone --recursive https://github.com/tsuyoshi-otake/m5stamp-p4-c6-linux.git
cd m5stamp-p4-c6-linux

# Start containerized build
./linux/build-m1.sh --profile m3-lab /path/to/output
```

This compiles:
1. ESP-IDF `boot-shim` for dual-core CPU1 release and SDIO initialization
2. Linux 6.18 SMP kernel with RISC-V P4 patches and Stamp-P4 Device Tree
3. Buildroot root filesystem with Busybox (vi, udhcpd), Dropbear SSH, MicroPython, and ESP-Hosted-NG (`esp32_sdio.ko`)
4. Combined image artifacts ready for flashing

---

## Acknowledgments & References

- [why2025-linux](https://github.com/mrbreaker/why2025-linux) by @mrbreaker for initial ESP32-P4 NOMMU bring-up reference.
- [Espressif ESP-Hosted](https://github.com/espressif/esp-hosted) for the SDIO Wi-Fi slave stack.
- The Buildroot and Linux kernel communities.
