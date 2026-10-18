# #37 root access over the network: hardware checks (2026-10-18)

Board: M5Stamp ESP32-P4 + C6 (SDIO), station mode, P4 USB Serial/JTAG on
COM10.  Only the P4 flash was written; the C6 was not touched.

Two ways to root, as on a normal Linux system:

- `su` with a per-device `root_password=` in `credentials.txt` (a setuid,
  su-only BusyBox, `/bin/busybox-su`, with `/bin/su` linked to it);
- SSH as root by key only (`dropbear -g` without `-w`, keys from the drive's
  `root_authorized_keys`), like Debian's `PermitRootLogin prohibit-password`.

## Image

A local build of branch `issue37-root-access` (da9b1bb) with `build72.sh`,
flashed over v0.3.1 with `flash-candidate.ps1 -PreserveBoot`, so the drive
kept its v0.3.1 `credentials.txt` (no `root_password=`):

```
2e1d40cf24ad04f04df24c07b11342049a7fe2f55e60e2ef21ec92bce9bbdc87  bootloader.bin
7937656f29960d53b63010b51836e233d092e067e320cde5778ec43e04218870  partition-table.bin
680d0ae34991de82b754cec10404d05025bbeec0307767b26461ef8d66bc27d6  boot-shim.bin
17f6ab23ab317a6f9b40c3d95f946258bfbc4877a41421d71bcb991abc202ce3  Image
2931588d8a00ee9def3f627e97b0c67b26f93cfdaeca1c3c882ab08282826b02  rootfs.squashfs
a12b18e1347ade36c499c79338ff7125bd4140bf56c64ecfedce8f93b4d3a852  easystick-stamp-p4.dtb
9353437c79648b69e3722b2b56880403ec87a5dce45932be9812fa6dae2e9729  boot.img
```

`busybox-su` is 110568 bytes.  The build checks that the setuid files in
`rootfs.squashfs` are exactly `/bin/busybox` and `/bin/busybox-su`.

## The main BusyBox was already setuid

The first build with that check failed because `/bin/busybox` is also
`-rwsr-xr-x`: Buildroot's `BUSYBOX_PERMISSIONS` installs it 4755, and it was
so in v0.3.1 and earlier.  On v0.3.1, `ping` started as `m5` runs with
`Uid: 1000 0 0 0` (euid root) while `sleep` runs with `1000 1000 1000 1000`:
the setuid bit works on these bFLT binaries, and BusyBox drops root for
applets that do not keep it.  SECURITY.md now names both files.

## Results

Drive edits were made on the board with a read-write loop mount of
`/tmp/boot.img`, as in the earlier records.  The root password used for
the tests was generated on the PC and written into `credentials.txt`; it
is not recorded here.

| Check | Result |
|---|---|
| First boot over the v0.3.1 drive | `check-boot-log.py` 11/11; `root_password=` generated and appended (`ES_CREDENTIALS NEW: root_password=... (added for su)`), `root password applied`, `boot commit: written to flash`, `ES_CREDENTIALS: saved to flash` |
| Files | `/bin/su -> busybox-su`; `/bin/busybox-su` and `/bin/busybox` `-rwsr-xr-x root root` |
| SSH as root, no `root_authorized_keys` | key, root password and m5 password all refused |
| `root_password=` changed in the live drive | applied by bootsync without a reboot |
| `su -` as `m5` over SSH, wrong password | `su: incorrect password` |
| `su -` with `root_password=` | `uid=0(root) gid=0(root) groups=0(root),10(wheel)`; `/proc/$$/status` `Uid: 0 0 0 0`; `dmesg` prints (193 lines); `traceroute -n -m 2 8.8.8.8` prints two hops; `/tmp/credentials.txt` readable |
| `root_authorized_keys` saved to the live drive | installed at once: `/root` and `/root/.ssh` `drwx------`, `authorized_keys` `-rw------- root root`; root login by key OK; by root password and by m5 password refused |
| Concurrent SSH sessions (`m5`, key auth, `read -t 100`, 10 tries 4 s apart) | 8 held (7 right after boot in `v0.3.0-feature-checks-2026-10-18.md`) |
| 3 resets with both files on the drive | 11/11 each; no `ES_CREDENTIALS NEW`; `root password applied`, `1 key(s) installed for root from root_authorized_keys`; root key login OK, root password logins refused |
| After the resets | `su` with the password set live still works (it was saved to flash) |
| Keys removed and `root_password=short`, then a reset | 11/11; `root_password= must be 8-64 characters; root login stays locked`; root SSH refused by key and password; `su` fails (`su: bad salt`, BusyBox's message for the locked `!` hash) |
| `root_password=` line deleted, then a reset | 11/11; a new one generated, applied and saved, as on the upgrade |

`config.txt` was back to its value before the tests (md5 unchanged) and the
test keys were removed from the drive.

## Additions (2026-10-19)

f8922b5 on `main`, local build `issue37-c`, flashed the same way
over the `issue37-b` drive:

```
5e939d85df20b09db7b52277c91b1ef202b26b6804b314f7fe69ce6ae29912f2  bootloader.bin
7937656f29960d53b63010b51836e233d092e067e320cde5778ec43e04218870  partition-table.bin
9af335180fa519f57a6abb37fe7101b149e947a2c3c17a280d41eb18bb1bb0b6  boot-shim.bin
17f6ab23ab317a6f9b40c3d95f946258bfbc4877a41421d71bcb991abc202ce3  Image
45536f8c5a41277db88cc437b0163a7977ea44f74b14298424883d2ae630c4f8  rootfs.squashfs
a12b18e1347ade36c499c79338ff7125bd4140bf56c64ecfedce8f93b4d3a852  easystick-stamp-p4.dtb
b9c8919bccd5c3c144b33df9736f18d977392498867aabb53908096ceeb99234  boot.img
```

The build log shows `0001-libbb-a-locked-password-never-matches.patch`
applied, and `CONFIG_FEATURE_SU_SYSLOG=y` in the busybox-su `.config`.

| Check | Result |
|---|---|
| Boot | `check-boot-log.py` 11/11 on every boot below (6 boots) |
| `su -` wrong, then right password | `su: incorrect password` about 3 s after the password (BusyBox's failure delay), then a root shell; `/run/messages` (`-rw------- root root`) has `auth.notice su: - pts/0 m5:root` and `auth.notice su: + pts/0 m5:root` |
| `root_authorized_keys` saved to the live drive, then deleted | root key login OK 1 s after the write; refused 1 s after the delete; `ES_SSH_KEYS: root_authorized_keys removed from the drive; keys for root revoked`, `/root/.ssh` empty, `/run/.es-keys-root` gone |
| The same with `authorized_keys` (m5) | m5 key login OK 6 s after the write, refused 6 s after the delete |
| A key added by hand as m5 over SSH (`$HOME/.ssh/authorized_keys`, no file on the drive) | kept across two drive imports; m5 key login still OK |
| `root_password=short` saved, then a reset | `root login stays locked`, `/etc/shadow` `root:!:`; `su` wrong password: `su: incorrect password` after 3.1 s and `su: - pts/0 m5:root` logged (was `su: bad salt` with no log or delay) |
| `ssh_password_auth=no` with only `root_authorized_keys`, then a reset | `ssh_password_auth=no ignored: no authorized_keys installed`, password-enabled Dropbear; m5 password login OK; root key OK; root by root password or m5 password refused |
| `ssh_password_auth=no` with both key files, then a reset | `starting key-only Dropbear on TCP/22 (ssh_password_auth=no)`; root key OK, m5 key OK; any password login refused (`allowed types: ['publickey']`); `su` from the m5 key session works and is logged |
| Both key files and the setting deleted from the live drive, then a reset | both revoked at once (`ES_SSH_KEYS ... revoked` for m5 and root); after the reset password-enabled Dropbear, m5 password login OK, every root login refused |

The drive's `config.txt` was back to its value before the tests (md5
unchanged) and no key file was left on it.

Each login also logs `dropbear: wtmpx_write: problem opening /var/log/wtmp`,
as in v0.3.1: the image has no wtmp/utmp and no `who`/`last`.

## Not covered

- A first boot from a blank drive (the generation code is shared with the
  m5 password; only the upgrade path was run here).  The v0.3.2 release
  check writes the whole 16 MB image, which includes a factory drive.
