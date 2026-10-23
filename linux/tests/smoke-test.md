# Hardware smoke test (#28)

Run this on every image before it is written up as a
`*hardware-validation-*.md` record.  Steps 1–2 are scripted; the rest are
typed on the serial console as root (the image has no `su`, and SSH refuses
root).  Record the output with the image hashes.

Serial capture uses `capture-boot.ps1`, which opens the port without toggling
DTR/RTS so the capture does not reset the board itself (see
[docs/troubleshooting.md](../../docs/troubleshooting.md) for Linux/macOS).

## 1. Cold boot

Flash, then start the capture and reset the board with `-Reset`:

```powershell
pwsh linux/tests/capture-boot.ps1 -Port COM10 -Output boot.bin -TimeoutSeconds 90 -Reset
python3 linux/tests/check-boot-log.py boot.bin
```

`check-boot-log.py` fails on:

- any `ES_* …FAIL` init marker (`ES_HOSTKEY GEN_FAIL`, `ES_OVERLAY FAIL`, …),
- `applet not found`, `can't execute`, `: not found`,
- `Kernel panic`, `Oops`, `BUG:`,
- `Incompatible ESP Host-firmware release detected` (C6 firmware mismatch),
- `wlan0 did not appear/associate`, `DHCP failed`, `wpa_supplicant failed`,
- a missing `ES_HOSTKEY INETD_START`, `wlan0 appeared`, or `SoftAP started` /
  `association complete`.

It prints the last kernel timestamp before inetd started, as a lower bound
for boot-to-SSH time. `CONFIG ERROR` lines are printed as warnings.

## 2. Plain reboot leaves no crash capsule (#19)

On the console: `reboot`.  Capture that boot (without `-Reset`) and check it
with `--after-reboot`, which also fails on `CRASH_CAPSULE VALID`:

```powershell
pwsh linux/tests/capture-boot.ps1 -Port COM10 -Output reboot.bin -TimeoutSeconds 90
python3 linux/tests/check-boot-log.py --after-reboot reboot.bin
```

## 3. Services and listeners

```sh
sysinfo
netstat -ltnu            # 0.0.0.0:22 LISTEN (inetd); udhcpd on :67 in SoftAP mode
ps | grep -E 'inetd|udhcpd|wpa_supplicant|(bootsync|mdns)'
```

Then from a Wi-Fi client: `ssh m5@192.168.4.1` (SoftAP) and
`ssh m5@stamp-p4.local` (mDNS, #25; the `hostname=` value if set).

## 4. Services come back (#17, #29)

```sh
killall inetd;   sleep 5; netstat -ltn | grep ':22 '      # respawned
killall udhcpd;  sleep 5; ps | grep udhcpd                 # SoftAP only
killall wpa_supplicant; sleep 10; ps | grep wpa_supplicant
```

## 5. /tmp cannot take the console down (#17)

`CONFIG_SHMEM` cannot be enabled on this NOMMU kernel (tmpfs is ramfs and
ignores `size=`), so S05 puts /tmp on ext2 on the 2 MB brd ramdisk.  The boot
log must show `ES_TMPFS OK: ext2 on /dev/ram0`; the ramfs fallback logs
`ES_TMPFS FAIL` and fails `check-boot-log.py`.

```sh
df /tmp                                   # /dev/ram0, about 1979 1K-blocks
grep MemAvailable /proc/meminfo
dd if=/dev/zero of=/tmp/fill bs=4096 count=1024   # must stop with ENOSPC
grep MemAvailable /proc/meminfo
rm /tmp/fill; netstat -ltn | grep ':22 '  # still listening
```

brd does not free pages when a file is deleted, so a full /tmp keeps about
2 MB pinned; a second fill must not lower MemAvailable further.

## 6. Soak

Leave the board up for 24 h with a client attached, sampling every 10 min:

```sh
while :; do date; grep MemAvailable /proc/meminfo; cat /proc/loadavg; read -t 600 _ 0<> /run/.soak 2>/dev/null; done
```

(`mkfifo /run/.soak` first; `read -t` on a FIFO avoids an exec per sample.)
MemAvailable must not trend downwards.

## 7. SDIO clock (images built with `SDIO_MAX_KHZ`, #21)

Record `dmesg | grep ES_SDIO_CLOCK` (root console; it is below the console loglevel),
time a 1 MB `scp` in both directions, and repeat steps 1, 4 and 6.  Only a
rate that passes all of them should become the DTS default.

## 8. pstore (default since #26; skip for `PSTORE=0` images)

```sh
echo c > /proc/sysrq-trigger     # deliberate panic; sysrq is only in this image
```

After the panic=5 reset, `ES_PSTORE OK` must report two records:
`dmesg-ramoops-0` with the panic, and `console-ramoops-0` ending in
`Rebooting in 5 seconds..`.  `rm /sys/fs/pstore/*` before each run so an
old record is not mistaken for a new one.
Repeat after `echo 0 > /proc/sys/kernel/panic` so the kernel hangs and the
watchdog resets the board; the record must survive that too (verified
2026-10-14).

## 9. Record

Write `linux/tests/<version>-hardware-validation-<date>.md` with the image
SHA-256s, the `check-boot-log.py` output of steps 1–2, and the results of
3–8.  The release workflow refuses assets whose hashes are not in such a
record.
