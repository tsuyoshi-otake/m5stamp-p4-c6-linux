# Autorun and MicroPython hardware validation (2026-10-17)

Issue #34.  Board: M5Stamp ESP32-P4 + C6 (SDIO), P4 USB Serial/JTAG on
COM10.  Only the P4 flash was written (`flash-candidate.ps1 -PreserveBoot` (-PreserveBoot was the opt-in then; since 2026-10-21 R2 preserving the boot slots is the default and the switch is a no-op; -WriteBoot writes the boot FS),
so the USB drive kept its contents); the C6 was not touched.

## Candidate

Built in Docker from commit 4e03b79 (`build-c68.sh --profile m3-lab`,
EXIT=0).  `/usr/bin/micropython` is the new `nommu` variant (431,308 bytes),
`/usr/bin/chpst` points at `busybox-tools`.

```
ac11cac958c4468e96ce821051fe7da545cafd53289d554b2d88c3d9279e4041  Image
fe27b66d7b2be07147928f6b1804969f0832c83f289777727a6d26a95fa1caaf  rootfs.squashfs
bd8b9ea5abe09cfb04f814140dd1473e17cc1db5cb456f0f0ffafb34796ebe57  boot.img
9892921c9d1068140f0b84351b52c535a84e9ad83d062ae7302d38e9f0182a63  boot-shim.bin
a12b18e1347ade36c499c79338ff7125bd4140bf56c64ecfedce8f93b4d3a852  esp32p4-m5stamp-p4.dtb
```

Every boot passed `linux/tests/check-boot-log.py` (11 checks) and printed
`ES_AUTORUN STAGED`.  Nothing was bind-mounted over the image: the tests ran
the files that ship in the rootfs.

## Test program

The drive was changed the way a USB host would change it: a root script
loop-mounted a copy of `/tmp/boot.img`, edited it and wrote it back.

`main.py` on the drive:

```python
import json, socket, time
import helper

print("main.py v1 start:", helper.NAME)

ai = [a for a in socket.getaddrinfo("example.com", 80, 0, socket.SOCK_STREAM)
      if a[0] == socket.AF_INET][0]
s = socket.socket(ai[0], ai[1], ai[2])
s.connect(ai[-1])
s.send(b"HEAD / HTTP/1.0\r\nHost: example.com\r\n\r\n")
print("http", s.recv(15))
s.close()

n = 0
while True:
    print(json.dumps({"tick": n, "uptime": helper.uptime()}))
    n += 1
    time.sleep(5)
```

`lib/helper.py` defines `NAME` and `uptime()` (reads `/proc/uptime`).
`config.txt` had no `autorun=` line.

## Results

| Check | Result |
|---|---|
| Drive edit picked up while running | `main.py` started as `m5` (`ps`: `m5 ... SN python3 main.py`) |
| `lib/` import, `json`, `socket` + DNS, `time.sleep` | log: `main.py v1 start: helper from lib/`, `http b'HTTP/1.1 200 OK'`, a JSON line every 5 s |
| Edit `main.py` (v1 → v2) | restarted by itself: `--- autorun: start main.py` / `main.py v2 start` |
| `autorun restart` / `status` as `m5` | `stopped pid 466`, `started main.py pid 489`, `running: pid 489` |
| `autorun=off` in `config.txt` | `--- autorun: stopped`, status `mode=off` |
| Wi-Fi during the autorun edits | `wlan0` `carrier_changes` stayed 0; no network reload |
| Power-on with `main.py` on the drive | `[bootsync] autorun: started main.py` about 20 s after reset; HTTP 200 and ticks in the log |
| Remove `main.py` and `lib/` | `not running`; `config.txt` md5 back to its original value |

One reset was sent within a few seconds of a drive edit, before bootsync had
written it to flash, and the board came back with the previous drive (and
`mode=off`).  That is the expected behaviour: the drive is saved only after
the `[bootsync] Saved to flash` line, the same as waiting for a USB stick
to finish writing.  After that line, the reset above autostarted the program.

## busybox-tools commands

`free`, `awk`, `xxd`, `nslookup` (A records), `tar czf`/`tar tzf`, `less`,
`top`, `wget` and `pgrep` are links to `busybox-tools` and ran as `m5`.
`/etc/motd` shows the Autorun lines.  `python3` reports `MicroPython
v1.22.2`.

`timeout` did not work on the 4e03b79 image: `timeout 2 sleep 8` took 8 s
and exited 0.  BusyBox re-executes itself on NOMMU to start the watcher,
but it put `-pPID` after the duration, where getopt32 had already stopped,
so the watcher never ran.  68b361f patches busybox-tools to re-exec as
`timeout -pPID DURATION PROG`.

## Candidate with the timeout fix

Built from 68b361f (EXIT=0, the patch applied to busybox-tools) and flashed
the same way; 11/11 boot-log checks passed.

```
7b1ae08da94d2055c62179b5975deedf34a29b62551e8eec82dac62bad05b57e  Image
3e56fb0e580f93cba2160c6ff39f6030af87fe2a56ee0d9ac4251f600309ebd2  rootfs.squashfs
7bd0ed9b2c69b3d005c6446f9667837ff23cf6a47b8c97b9ce1bbe8313d83499  boot.img
3349c37e1905e9a73b7b8d92b0f15d182b3866fb64d9ddc460ee5668c62718e0  boot-shim.bin
a12b18e1347ade36c499c79338ff7125bd4140bf56c64ecfedce8f93b4d3a852  esp32p4-m5stamp-p4.dtb
```

| Check | Result |
|---|---|
| `timeout 2 sleep 8` | `Terminated`, exit 143 after 2 s |
| `timeout 5 sleep 1` | exit 0 after 1 s |
| `timeout -s KILL 1 sleep 4` | `Killed`, exit 137 |
| Watchers left over | none |
| `main.py` + `lib/` on the drive | started (`main.py v1 start: helper from lib/`, `http b'HTTP/1.1 200 OK'`), stopped when removed; `config.txt` md5 back to its original value |
