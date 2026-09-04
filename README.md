# M5Stamp ESP32-P4 + C6 SDIO Linux

[![License](https://img.shields.io/badge/license-GPLv2%20%2F%20MIT%20%2F%20Apache-blue.svg)](LICENSE)
[![Platform](https://img.shields.io/badge/platform-ESP32--P4%20%2B%20ESP32--C6-orange.svg)](https://m5stack.com)

A reproducible native RISC-V NOMMU Linux 6.18 distribution for the **M5Stack Stamp-P4** carrier board paired with **Stamp-AddOn C6** (via SDIO interface).

Features out-of-the-box Wi-Fi (`wlan0`) via Espressif ESP-Hosted-NG and stable Dropbear SSH server.

---

## Highlights & Verified Hardware Capabilities

- **SoC Target**: ESP32-P4 (RV32IMAC / ilp32, 16 MiB Flash, 32 MiB PSRAM).
- **Wi-Fi Companion**: ESP32-C6 (M5Stack Stamp-AddOn C6) connected via high-speed 4-bit SDIO.
- **Kernel**: Linux 6.18-rc (NOMMU) with essential ESP32-P4 SoC stabilization patches:
  - Signal delivery `mcause` hardening (prevents interrupt masking corruption upon signal return).
  - L2 cache / DMA reentrancy & cache thunk fixes (prevents silent memory corruption during cache flushes).
  - Systimer 52-bit multi-word atomic read and clock event missed-alarm prevention.
  - DW-MMC host controller driver enhancements with software DTO and recovery timers.
- **Networking**:
  - Stable SDIO datapath with ESP-Hosted-NG.
  - Interactive shell and non-interactive `ssh <host> <cmd>` (Paramiko / OpenSSH / Dropbear `dbclient`) verified rock-solid without watchdog timeout (`rst:0x7`).

---

## Technical Deep Dive: The "SSH Wedge" & Kernel Stabilization

Bringing up native Linux on early RISC-V NOMMU silicon often uncovers subtle interaction issues between hardware errata and kernel subsystems.

During initial development, a critical failure known as the **"SSH Wedge"** manifested: executing any non-interactive SSH command (`ssh pi@host id`) or child process termination caused an immediate communication lockup, followed by a hardware watchdog reset (`rst:0x7`) ~120–160s later.

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
│   ├── dts/               # Device Tree Source (Stamp-P4)
│   ├── kernel-patches/    # Target-specific Linux kernel patches
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

## Hardware Pinout (Stamp-P4 <-> Stamp-AddOn C6)

| Signal | ESP32-P4 Pin | ESP32-C6 Pin | Function |
| :--- | :--- | :--- | :--- |
| **C6_PU / RESET** | GPIO 42 | CHIP_PU | Reset line for C6 slave |
| **SDIO_CLK** | GPIO 43 | GPIO 19 | SDIO Clock |
| **SDIO_CMD** | GPIO 44 | GPIO 18 | SDIO Command |
| **SDIO_DAT0** | GPIO 45 | GPIO 21 | SDIO Data 0 |
| **SDIO_DAT1 / IRQ**| GPIO 46 | GPIO 22 | SDIO Data 1 / Interrupt |
| **SDIO_DAT2** | GPIO 47 | GPIO 23 | SDIO Data 2 |
| **SDIO_DAT3** | GPIO 48 | GPIO 15 | SDIO Data 3 |

---

## Getting Started

### 1. Clone with Submodules

```bash
git clone --recursive https://github.com/tsuyoshi-otake/m5stamp-p4-c6-linux.git
cd m5stamp-p4-c6-linux
```

If you already cloned without submodules:
```bash
git submodule update --init --recursive
```

### 2. Build the System

The build process is fully automated and uses Docker to provide a hermetic Debian build environment:

```bash
# In Bash:
./linux/build-m1.sh --profile m3-lab /path/to/output
```

This compiles:
1. ESP-IDF `boot-shim` (ROM bootloader -> secondary bootloader)
2. Linux 6.18 kernel with RISC-V P4 patches and Stamp-P4 Device Tree
3. Buildroot root filesystem with Busybox, Dropbear, and ESP-Hosted-NG kernel module (`esp32_sdio.ko`)
4. Combined image artifacts ready for flashing

### 3. Flash to Hardware

Connect the Stamp-P4 via USB-C (USB-Serial/JTAG on GPIO24/25) and flash using PowerShell:

```powershell
.\linux\flash-candidate.ps1 -ComPort COM10 -ArtifactsDir /path/to/output
```

---

## Acknowledgments & References

- [why2025-linux](https://github.com/mrbreaker/why2025-linux) by @mrbreaker for initial ESP32-P4 NOMMU bring-up reference.
- [Espressif ESP-Hosted](https://github.com/espressif/esp-hosted) for the SDIO Wi-Fi slave stack.
- The Buildroot and Linux kernel communities.
