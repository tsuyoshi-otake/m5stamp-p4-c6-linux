# `vi` freeze: NULL pointers in system calls (2026-10-17)

Board: M5Stamp ESP32-P4 + C6 (SDIO), P4 USB Serial/JTAG on COM10, joined to a
home AP in station mode.  Only the P4 flash was written; the C6 was not touched.

## Symptom

`vi` with no file name froze the whole board: over SSH the session stopped
answering, Wi-Fi went quiet, and the watchdog reset the chip one to two minutes
later.  On the serial console the reset came within about 7 s.  `vi file.txt`
worked.  pstore kept only an `ES_WDT` line, even with `dmesg -n 8`.

## Cause

BusyBox 1.37 `vi` with no file name reaches `file_insert(NULL, ...)`, which
calls `open(NULL, O_RDONLY)`.

A test program (`write(1, p, 4)`, `open(p)`, `stat(p)` and a user-mode load
`*(int *)p`, with an address from the command line) showed:

| Call | Before (cand-AR, ed2f534) |
|---|---|
| `*(int *)0` in user mode | SIGSEGV, as expected (`unhandled signal 11 code 0x2`) |
| `write(1, NULL, 4)` | board froze, watchdog reset |

With `CONFIG_MMU` off, riscv uses `asm-generic/uaccess.h`.  `copy_from_user()`
is a plain `memcpy()`, `__ex_table` is empty, and the generic `__access_ok()`
returns true for every address on NOMMU.  So the kernel itself loads from the
user's pointer.  The resulting access fault in `memcpy()` is not recovered.  One
run that was halted over JTAG left a pstore oops: `Oops - illegal instruction`
at `epc 0x48347d84` (inside the freed init area), `badaddr 0x0000f970`, which
is the upper half of a 32-bit instruction.  The other runs left nothing.

## Fix

`linux/kernel-patches/0071-nommu-access-ok.patch` gives this SoC its
own `__access_ok()`.  A range is accepted only if it starts in
`[phys_ram_base, high_memory)` (the PSRAM Linux manages) or in the mtd-rom
flash window that physmap registers.  Everything else returns `-EFAULT`
before the kernel touches it.

The ROM window is needed: the first build accepted PSRAM only, and `modprobe`
and `insmod` failed with `Bad address` at boot (no `wlan0`, `ES_USB_GADGET
FAIL`).  BusyBox maps the `.ko` from the squashfs, NOMMU `mmap()` returns a
pointer into the flash window, and `init_module()` reads from it.

Only the start of the range is checked.  `strncpy_from_user()` and
`strnlen_user()` call `access_ok()` with their whole limit (`PATH_MAX`, or
`MAX_ARG_STRLEN` for execve) and stop at the NUL.  The top NOMMU pool ends at
`high_memory`, so an end check would reject short strings there.

## Results (cand-AK2, 9336a15)

`check-boot-log.py`: 11/11.

Test program on the serial console:

| Call | Result |
|---|---|
| `write(1, 0x0, 4)` | `-1 errno=14 (Bad address)` |
| `open(NULL)` | `-1 errno=14` |
| `stat(NULL)` | `-1 errno=14` |
| `write(1, 0x10, 4)` | `-1 errno=14` |
| `write(1, 0x3ff00000, 4)` | `-1 errno=14` |
| `write(1, 0x50000000, 4)` (peripherals) | `-1 errno=14` |
| `write(1, 0x4a000000, 4)` (end of PSRAM) | `-1 errno=14` |
| `write(1, 0x40030000, 4)` (mtd-rom) | `4`, wrote `hsqs` (squashfs magic) |
| `write(1, 0x49fffffe, 4)` (last PSRAM bytes) | `4` |
| `*(int *)0` in user mode | SIGSEGV |

`vi`:

| Where | Result |
|---|---|
| Serial console, `vi` with no file | `- No file 1/1 100%`, `:q!` back to the shell, board still up |
| SSH pty 120x30, `vi` with no file | insert `hello`, `:w /tmp/vt.txt` wrote 1 line / 6 chars, `:q`, file read back |
| SSH pty 240x63, `vi` with no file | screen drawn, insert works, `:w` refused because the file existed (normal), `:q` refused for the unsaved buffer (normal) |

The modules loaded (`esp32_sdio`, `g_mass_storage`), STA associated, and SSH
password login worked.

## Command sweep over SSH (user `m5`)

All exit 0 with the expected output: `id`, `whoami`, `hostname`, `uname -a`,
`date`, `pwd`, `ls`, `cat /proc/loadavg`, `ps`, `df -h`, `du`, `mount`, `lsmod`,
`ifconfig`, `netstat -tln`, `route -n`, `ping <gateway>` ("is alive!"), `sort`,
`uniq`, `grep`, `sed`, `cut`, `tr`, `head`, `tail`, `wc`, `xargs`, `find`,
`expr`, `seq`, `md5sum`, `sha256sum`, `xxd`, `dd` (64 KiB), `cp`, `cmp`, `mv`,
`ln -s`, `readlink`, `realpath`, `mkdir -p`, `rmdir`, `mktemp`, `touch`,
`chmod`, `mkfifo`, `rm`, `printf`, `test`, `[`, `env`, `which`, `sleep`,
`nice`, `nohup`, `yes`, `basename`, `dirname`, `python3 < script`,
`micropython -c`, `dropbearkey`, `ssh -V`, `tcpdump --version`, `cryptpw`,
`more`.

Interactive, over an SSH pty: the `python3` REPL (`6*7` → `42`, Ctrl-D back to
the shell), Ctrl-C on `sleep 30`, `cat` echo and Ctrl-D, `more`, and a nested
`sh` with `exit`.

Findings:

- `getconf` printed `applet not found`.  `/usr/bin/getconf` was the ELF build
  from uClibc's utils, which this bFLT-only kernel cannot run, so the shell
  fell back to BusyBox.  The m3-lab post-build now removes it.  It was the only
  ELF executable in the rootfs; the kernel modules are the only ELF files left.
  `getconf` is still unavailable and now reports `can't execute 'getconf': No
  such file or directory`, like any other missing command.
- `ping` is BusyBox without `FEATURE_FANCY_PING`: `ping HOST` works, while
  `-c` and `-W` are rejected (`bad address '-c'`).
- `dmesg` as `m5` is refused (`klogctl: Operation not permitted`), as
  configured.
- `tcpdump` needs a 2.2 MB contiguous block.  It failed once
  (`nommu: Allocation of length 2273280 ... failed`) and ran on the next try.
  This is the known pool limit, not a regression.

## Re-check on cand-GC (707c774)

Same kernel source, rootfs without `getconf`.  `check-boot-log.py`: 11/11.
After restoring the station config, SSH password login worked and the same
sweep gave 61 commands with exit 0.  The two non-zero results were `ping -c`
and `getconf`, both described above.

## Not covered

- `top` and `less` are not in the image.
- Over the serial console, only `vi` was exercised interactively.
- The released v0.2.3 image was not re-tested.  It has the same kernel
  accessors and BusyBox, so it is expected to freeze the same way.
