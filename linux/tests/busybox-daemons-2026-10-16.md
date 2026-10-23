# busybox-daemons: a small BusyBox for the resident daemons (2026-10-16, #23)

Board: M5Stamp ESP32-P4 + C6 on COM10; only the P4 flash was written.

## Change

bFLT programs on this NOMMU kernel are loaded to RAM (flags `RAM|GOTPIC`)
and do not share their text, so every process of the full BusyBox takes a
496 KB region of the 10 MB user pool (`0070`).  XIP is not possible: RISC-V
bFLT code reaches its data PC-relative, and there is no RISC-V FDPIC
toolchain.

The m3-lab image now builds a second BusyBox,
`package/busybox-daemons`, from the same source and patches with
`busybox-daemons.config` merged over `allnoconfig`: init (with inittab),
inetd, syslogd and udhcpd, no shell.  `post-build.sh` points
`/sbin/init`, `/usr/sbin/inetd`, `/sbin/syslogd` and `/usr/sbin/udhcpd` at
`/bin/busybox-daemons`; the full BusyBox no longer has inetd, syslogd or
udhcpd, so its `PREFER_APPLETS` shell cannot run them internally.
`halt`, `reboot` and `poweroff` stay in the full BusyBox and signal PID 1.
`build-m1.sh` checks the links and the full BusyBox's config, and the
package fails its configure step when `oldconfig` drops any listed option.

| Binary | File | bFLT text | data+bss | Region in the pool |
|---|---|---|---|---|
| `/bin/busybox` | 453 792 B | | | 480 KB |
| `/bin/busybox-daemons` | 159 116 B | 106 KB | 67 KB | 192 KB |

## Image

Local m3-lab candidate from 3e8ed98, DTS with the 40 MHz SDIO cap (the
request stays 5 MHz).  The baseline is the previous candidate from
d4495e6 (same kernel patches and DTB, full BusyBox for every daemon).

| File | sha256 |
|---|---|
| `rootfs.squashfs` | `343383d5a764d0db16111a0027e4bc03fb82b19f8c2acad4a65adb9c4425fcca` |
| `Image` | `f7a612cfca29ff1d7737f777955de4def7e818034f0d6b8c782fb30453e0f2ca` |
| `esp32p4-m5stamp-p4.dtb` | `a12b18e1347ade36c499c79338ff7125bd4140bf56c64ecfedce8f93b4d3a852` |

## Pool use

Sum of the distinct user regions in `/proc/<pid>/maps` that fall in the
two pool regions, at idle after boot:

| Mode | Baseline | busybox-daemons | Difference |
|---|---|---|---|
| SoftAP (init, inetd, syslogd, udhcpd resident) | 5960 KB | 4688 KB | −1272 KB |
| Station (udhcpd not running) | 6352 KB | 5360 KB | −992 KB |

`MemAvailable` stayed at 8708–8712 kB in SoftAP mode on both images: the
pool is reserved memory and `/proc/meminfo` does not count it.

## Concurrent SSH sessions

Station mode; ten `ssh … "echo ok; sleep 100"` sessions opened 4 s apart,
counting those that printed `ok`.  Each session needs Dropbear (a 600 KB
region) and a BusyBox shell; the rest fail with `nommu: Allocation of
length 614400 from process … (dropbear) failed` and the client sees
`Connection reset by peer`.

| Image | Runs |
|---|---|
| Baseline | 5, 4, 4, 4 |
| busybox-daemons | 5, 6, 5, 5 |

## Functional checks (busybox-daemons image)

| Check | Result |
|---|---|
| `check-boot-log.py` after each of the two flashes | 11/11 |
| `/etc/inittab` read (no askfirst on `/dev/tty2`–`tty4`) | yes |
| syslogd writes `/run/messages` (`udhcpd[…]: started`, init's `starting pid`) | yes |
| udhcpd in SoftAP mode, listening on UDP 67 | yes |
| Station mode: raw TCP 295–345 KB/s, SSH 209–219 KB/s at 5 MHz | same as before |
| `killall inetd`: respawn restarts it, SSH works | yes |
| `reboot` from the console | board back up, daemons running |
| 10 resets (`capture-boot.ps1 -Reset`) | 11/11 each, no `Allocation of length`, no tty errors |

init logs with an empty ident (`daemon.info : starting pid …`):
`FEATURE_INIT_MODIFY_CMDLINE` overwrites argv[0] `/sbin/init` with `init`,
and `applet_name` points past it.  The full BusyBox had the same option
and the same link, so this is unchanged.  syslogd is started once by
`rcS` and not supervised, as before.

## Not covered

- A soak of hours.
- A DHCP lease to a real client in SoftAP mode (the PC stays on the
  home Wi-Fi); udhcpd was checked as started and listening only.
