# Saving the USB drive from Linux (patch 0072) — hardware check, 2026-10-17

Image: main at 93ffa84 (m3-lab, C68), flashed to the M5Stamp P4 over COM10.

## What changed

- `drivers/misc/esp32p4-bootflash.c` (0072) writes the staged PSRAM request to the
  inactive A/B slot and metadata sector, as boot-shim does at reboot.
- `boot-commit` asks the kernel to do that after staging (exit 0 = in
  flash, 2 = staged for the next reboot).
- `bootsyncd` saves the drive 3 s after the host stops writing, with no
  eject and no reboot, and applies network, password and keys live.

## Results

| Check | Result |
|---|---|
| `check-boot-log.py`, fresh flash | 11/11 PASS |
| Driver probe | `bootflash: flash id 464018, /boot commits without a reboot` |
| Helper commit from a shell (config.txt edit) | `boot update committed: generation=2 slot=0`; sysfs shows gen 2 at once |
| Reboot | boot-shim `boot selection: generation=2 slot=0` |
| credentials.txt written into the live drive image (`/tmp/boot.img`, as a host write) | bootsync: `Saved to flash` then `m5 password applied`; kernel `generation=3 slot=1` |
| SSH password login over Wi-Fi STA, no reboot | OK |
| Hardware reset (RTS) | `boot selection: generation=3 slot=1`, 11/11 PASS, SSH login OK |
| `/sys/kernel/board/boot_commit` as user m5 | Permission denied (0600, root only) |

Earlier spike on the same board (bfmod): 256 KiB slot write 1.24 s, longest
IRQ-off section 18 ms (sector erase), no ping loss, wlan0 error counters 0.

## Build fix found on the way

The first image still carried the Oct 15 `boot-commit`: Buildroot copies
a local-site package's `src` only when its build directory is created, so an
incremental build never picked up the change.  `build-m1.sh` now hashes each
local package tree and dircleans that package when it changes (4cc8b14, 93ffa84).
A clean CI build was not affected.

## Not covered

- Edits from a real Windows/macOS host over USB-A.  The host write was emulated by
  a loop mount of `/tmp/boot.img` on the board; the USB transport and the host's
  write pattern (several bursts, FAT cache flush timing) are untested.
- Power loss during a kernel flash write (the shim's CRC and generation checks
  are designed to fall back to the previous slot, but this was not pulled).
