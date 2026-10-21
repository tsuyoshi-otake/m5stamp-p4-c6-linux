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
flashed over v0.3.1 with `flash-candidate.ps1 -PreserveBoot` (-PreserveBoot was the opt-in then; since 2026-10-21 R2 preserving the boot slots is the default and the switch is a no-op; -WriteBoot writes the boot FS), so the drive
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

Each login also logged `dropbear: wtmpx_write: problem opening /var/log/wtmp`,
as in v0.3.1: the image had no wtmp/utmp and no `who`/`last`.  That is
fixed below.

## Login records (2026-10-19)

bc6fcca on `main`, local build `issue37-e`, flashed the same way:

```
206b844ac20b92d7ad9779ed2f19e5252f906bc56eef8300e676c62be357a494  bootloader.bin
7937656f29960d53b63010b51836e233d092e067e320cde5778ec43e04218870  partition-table.bin
582f3a5624e96321298e4ed8aafe6186a3872f38b5cb98344ec446f4cf39de60  boot-shim.bin
17f6ab23ab317a6f9b40c3d95f946258bfbc4877a41421d71bcb991abc202ce3  Image
b03d3817d1419fa83690e7a3e8dd2d0270a6b938331f48cc70304a3f3d67f365  rootfs.squashfs
a12b18e1347ade36c499c79338ff7125bd4140bf56c64ecfedce8f93b4d3a852  easystick-stamp-p4.dtb
fad995bf9548815f6ef7430a1e5f2d3d7b0fef13ddfd5b14c83b7aaa89d118a1  boot.img
```

Before the change, creating the two files by hand on the `issue37-c` image
was enough for Dropbear to record a login (384-byte records, 768 bytes per
login and logout) and the warning stopped.  `/tmp` (and so `/var/log`) is
`/dev/ram0`, a 1979 KB ext2 ramdisk with 1709 KB free, shared with `/home`.

| Check | Result |
|---|---|
| Boot | `check-boot-log.py` 11/11; `/var/run/utmp`, `/var/log/wtmp`, `/var/log/wtmp.1` empty, `-rw-rw-r-- root utmp` |
| Two terminal sessions held, commands run as m5 from a third connection | `who` and `w` list both (`pts/0`, `pts/1`, login time, client address); `users` prints `m5 m5`; `uptime` says `2 users`; `last` shows both `still logged in` |
| After both exit | `who` prints nothing; `last` shows them with logout times |
| Commands run without a terminal (`ssh host cmd`) | not recorded, as in OpenSSH |
| Rotation: 2 + 18 + 18 paced logins (72 records, 29184 bytes), then 8 logins at once | rotated once inside the burst: `wtmp.1` 32640 bytes (85 records), `wtmp` 2688 bytes (7 records); 85 + 7 = 92, every record kept, both sizes multiples of 384; no `wtmpx` warning in `/run/messages`; both files still `root:utmp 0664` |
| `last -f /var/log/wtmp.1` as m5 | 46 logins (2 + 36 + 8) |
| m5 writing `wtmp` or `wtmp.1` | `Permission denied` |

The first build of this change (`issue37-d`) renamed `wtmp` to `wtmp.1`
instead.  On the board every rename failed with `Operation not permitted`
and `wtmp` grew past the limit: the Dropbear session process runs as the
user (`Uid: 1000 1000 1000 1000`, `Gid: 1000 1000 43 1000`, no
capabilities) and switches its effective group to `utmp` only while it
writes the records, so it cannot rename in the sticky `/tmp`.  The patch
now copies `wtmp` over `wtmp.1` and empties it, under `flock()`.

### Leaks

Resource use was read from the serial console between batches of logins
(pool = distinct `/proc/*/maps` regions in the NOMMU process pool, which
`MemAvailable` does not count):

| Point | pool | utmp | wtmp + wtmp.1 | processes | open fds |
|---|---|---|---|---|---|
| Idle, before | 6472K | 3072 | 4224 + 32640 | 48 | 64 |
| After 18 / 36 / 54 paced logins | 6472K each | 3072 | grows, rotates at 54 | 48 | 62-64 |
| After 8 more at once | 6472K | 3072 | 19200 + 32640 | 48 | 64 |
| After 10 sessions killed with `kill -9` on their Dropbear and 6 more logins | 6488K | 3072 | 1152 + 32640 | 48 | 64 |

The pool grew by 8K during each of the two `kill -9` rounds, which ran
many more console commands than the login batches.  A per-process
breakdown before and after the second round put the 8K in the serial
console's own shell (pid 289, `-/bin/sh` on ttyGS1, which ran the test
commands: 516K, then 524K); no other long-lived process changed and no
Dropbear stayed behind.  The first 8K was not broken down.

A session whose Dropbear is killed leaves its login record (no logout
record), and `who` keeps showing it until the next login on the same pts,
as on other systems.  uClibc's `pututxline()` reuses the slot with the same
`ut_id`, so `utmp` stayed at 3072 bytes (8 slots) through all of it.

An independent read of the code by a second agent found no leak: every fd
in the patch is closed on all paths, the `flock()` goes with the fd, the
heap use (`login_alloc_entry`, uClibc's one `static_utmp`) is in the
per-connection process only, and both files are bounded.  Two minor
points it noted, neither a leak: `wtmp.1` is truncated before `wtmp` is
opened for reading, so if that second open failed the old records would
be lost; and `utmp` writes are not locked, so two logins at the same moment
could briefly show a duplicate in `who`.

BusyBox inetd stops listening for 60 s after 20 connections within a
minute (`inetd.conf`, #25), so the logins above were paced; an unpaced
first try hit `22/tcp: too many connections, pausing` as designed.

## Not covered

- A first boot from a blank drive (the generation code is shared with the
  m5 password; only the upgrade path was run here).  The v0.3.2 release
  check writes the whole 16 MB image, which includes a factory drive.
