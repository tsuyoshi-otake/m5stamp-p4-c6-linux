# Functional and MicroPython checks (2026-10-19)

Board: M5Stamp ESP32-P4 + C6 (SDIO), P4 USB Serial/JTAG on COM10.  Only the
P4 flash was written; the C6 was not touched.  No SSID, PSK, password or
client address is quoted here.

Two local m3-lab builds (`build-c68.sh --profile m3-lab`, flashed with
`flash-candidate.ps1 -PreserveBoot` (-PreserveBoot was the opt-in then; since 2026-10-21 R2 preserving the boot slots is the default and the switch is a no-op; -WriteBoot writes the boot FS)):

- **e**: `main` at a34060a (the #39/#40 changes).
- **f**: e plus c07cc4f (loopback) and 90c4547 (MicroPython stack).  The DTB
  is unchanged (`a12b18e1…`); the Image differs only because the kernel was
  rebuilt (the build time is embedded).

```
50e9912b2f5f07bb48e4e93c14ae1c62905dd7f04f4f94af75c570ca3591819e  bootloader.bin
7937656f29960d53b63010b51836e233d092e067e320cde5778ec43e04218870  partition-table.bin
5da85fc38a6aafb5c0bb202e62df779a9918beb256e778877d2d387a2240ce8c  boot-shim.bin
f4c8bb020b8913206683dec2b6ea3a255a06aeed40822dad8769150471bb4bd0  Image
d5ce1649067ac95e463b7a4e8d6c189b87e9ad1ea9cc90a634a12e592f422b94  rootfs.squashfs
a12b18e1347ade36c499c79338ff7125bd4140bf56c64ecfedce8f93b4d3a852  esp32p4-m5stamp-p4.dtb
e4e8b7ba55073654f9870b3df947255e4a4b28ff062bf20f87415c3b98cb0756  boot.img
```

`check-boot-log.py`: 11/11 on both.

## Two bugs found on build e

| Symptom | Cause | Fix (build f) |
|---|---|---|
| A MicroPython UDP socket bound to `127.0.0.1` raised `OSError 99` (EADDRNOTAVAIL) | `lo` was DOWN with no address: the m3-lab `S40network` replaces Buildroot's, whose `ifup -a` was the only thing that configured it | c07cc4f: `ifconfig lo 127.0.0.1 netmask 255.0.0.0 up` on `start`.  On f: `lo` is `UP,LOWER_UP` with `127.0.0.1/8`, and the loopback test passes |
| A plain recursive function crashed with `Segmentation fault` (exit 139) at a depth of about 130–150; depth 120 was fine | The unix port checks recursion against a 40000-byte limit (`stack: 672 out of 40000` in `micropython.mem_info()`), but the bFLT stack was 32 KB, so the check never fired | 90c4547: bFLT stack 48 KB, limit 36 KB.  `bss_end + stack` = 491040 bytes, under the 496 KB build check and in the same 512 KB block.  On f: depth 140 returns, 150, 200 and 100000 raise `RuntimeError: maximum recursion depth exceeded` |

`hashlib` has `sha256` only: the unix port builds `sha1` and `md5` with its
TLS library, which this image leaves out.  That is now in the README; it is
not a regression.

## MicroPython (`python3`, v1.22.2), builds e and f

`mpytest.py` (run as `m5` over SSH):

| Check | Result |
|---|---|
| Imports of the 21 modules the README lists | all OK |
| `_thread`, `ssl`, `ffi`, `framebuf`, `cmath`, `deflate` | `ImportError`, as documented |
| `json`, `re`, `struct`, `math`, `collections`, `heapq`+`random`, `array`+`io`, `errno` | OK |
| `hashlib.sha256(b"abc")` | the standard digest |
| File I/O in `/tmp`, `os.getcwd()`, `$USER` | OK (`/home/m5`, `m5`) |
| `time.sleep_ms(200)` | 202 ms; `localtime()` is the NTP-set date |
| DNS + HTTP GET of example.com (IPv4 socket) | `HTTP/1.1 200 OK` |
| UDP on 127.0.0.1 with `select.poll` | e: `OSError 99`; f: OK |
| `asyncio.gather` of three sleeps | finish in delay order |
| 200 × 1 KB `bytearray`, then `gc.collect()` | OK; heap 1 MB, about 1013 KB free afterwards |
| Recursion | e: SIGSEGV; f: `RuntimeError` (above).  In a closure the limit is reached at depth 100 |

Total on f: 44 of 45 pass; the one failure is the closure reaching the limit
at depth 100, which is the new limit working (the README gives about 140).

`mpybench.py` (build f; e within ±15%):

| Benchmark | Time |
|---|---|
| integer loop, 200 000 iterations | 1946 ms |
| first 1000 primes (trial division) | 226 ms |
| float sum, 50 000 terms | 538 ms |
| `"%d:%s"` formatting, 5000 strings + join | 224 ms |
| dict insert, 5000 keys | 1232 ms |
| sha256 over 256 KB | 67 ms |
| 50 × 100 KB of `bytearray` + `gc.collect()` | 360 ms |

With 20 000 strings in one list the string benchmark ran out of the 1 MB
MicroPython heap (`MemoryError`); that is the heap size, not a fault.

## Autorun (Pico-style), builds e and f

Files were written into the live drive image from the serial console, as in
the earlier records.

| Step | Result |
|---|---|
| `armod.py` + `main.py` (loops every 2 s, `import armod`, writes `/run/autorun/state.json`) | `running`, `python3 main.py` as `m5`; the import worked; `state.json` counted up; the log shows each tick |
| A new `main.py` that exits after three ticks | replaced the running one within the 25 s window; ran to `AR v2 done`; `not running` afterwards and still after another 35 s |
| Both files deleted | `not running`; the drive back to its four files; config.txt md5 unchanged |

## Everyday commands and Wi-Fi

| Check | Result |
|---|---|
| `cmdcheck.py` as `m5` (both builds) | `vi`, `less`, `top` in a pty and the everyday commands OK; `python3` fails only on the script's `os.uname()` case, as documented |
| 3 × 10 MB `wget` over STA (build e) | all exit 0, 55–59 s each; `carrier_changes` 0 |
