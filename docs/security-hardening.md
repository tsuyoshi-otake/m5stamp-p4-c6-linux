# Secure Boot and flash encryption (#32)

The release images leave both off (`esptool get-security-info`: `Secure Boot:
Disabled`, `Flash Encryption: Disabled`). That is deliberate for a development
board: the flash can be read back and rewritten from ROM download mode, which
is the recovery path for every failed update. Anyone with physical access can
therefore read the boot FAT (Wi-Fi PSK, `credentials.txt`) and write their own
firmware.

This page is a plan for a product build. **None of it has been tried on this
board.** Every step below burns eFuses, which cannot be undone; a mistake
leaves the chip unable to boot anything but correctly signed images, or not at
all.

## What would have to change

| Piece | Today | With Secure Boot v2 + flash encryption |
|---|---|---|
| Second-stage bootloader and boot shim (`linux/boot-shim`) | unsigned ESP-IDF app | signed with the Secure Boot v2 key (`CONFIG_SECURE_BOOT_V2_ENABLED`, `CONFIG_SECURE_SIGNED_APPS_*`) |
| `kernel`, `rootfs`, `dtb` partitions | raw data, CRC checked by the shim | the shim must verify a signature over them before jumping to Linux; Secure Boot v2 itself only covers the bootloader and the app |
| Flash contents | plaintext | XTS-AES; partitions marked `encrypted` in `partitions.csv` |
| Linux rootfs reads | flash MMU window (`mtd-rom`, `root=/dev/mtdblock0`) | still the MMU window, which decrypts transparently, so squashfs needs no change |
| Boot FAT A/B commit | written by the shim through `esp_partition_write` | the same call encrypts when the partition is marked `encrypted` |
| Recovery | ROM download mode, `esptool write_flash` | Release mode disables plaintext reads and unsigned writes; recovery needs encrypted, signed images (`esptool --encrypt`, or a host-side key in Development mode) |
| `crashlog` partition, `tools/readback-*.ps1` | read back as plaintext | read back as ciphertext unless read through the running firmware |

The kernel and rootfs are not covered by ROM Secure Boot. Without the shim
check in the second row, an attacker can still replace Linux while the boot
chain stays "secure".

## USB entry points

Signed, encrypted firmware still leaves the development conveniences that
give anyone with a USB cable the owner's rights. A product build would also:

- turn off the USB drive, or export it read-only, and keep `credentials.txt`
  off it;
- turn off autorun of `main.py`/`main.sh`/`main` from the drive;
- put a login on the serial console (`getty`), or turn the console off;
- accept updates through the boot FAT A/B commit only when they are signed.

Process isolation is not part of this plan: user programs run in M-mode and
share one memory pool (README, *Security Design*), as on most ESP32 products,
so the firmware is one trust domain.

## Order of work

1. Add a signature check for `kernel`/`rootfs`/`dtb` to the boot shim (for
   example an ECDSA signature in a small trailer, the public key compiled into
   the signed shim) and test it with Secure Boot **off**.
2. Mark the data partitions `encrypted`, enable flash encryption in
   **Development** mode on a spare board, and confirm boot, rootfs reads, the
   boot FAT commit, crash capsules and `ES_FACTORY_RESET`.
3. Enable Secure Boot v2 on the same spare board with a test key.
4. Only then consider Release mode, which also disables JTAG and the ROM
   download features that the recovery tools here depend on.

Keep the signing key and the flash encryption key (if generated on the host)
outside the repository; `tools/check-no-secrets.py` does not know about them.

References: ESP-IDF *Secure Boot v2* and *Flash Encryption* guides for
ESP32-P4, matching the ESP-IDF version in `versions.lock.json`.
