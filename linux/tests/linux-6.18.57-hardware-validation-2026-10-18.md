# Linux 6.18.57 hardware validation (2026-10-18, #36 step 1)

Board: M5Stamp ESP32-P4 + C6 (SDIO), P4 USB Serial/JTAG on COM10.  Only the
P4 flash was written; the C6 was not touched.

This is the first step of #36: the 6.18 stable update from 6.18.35 to
6.18.57, before the move to 7.2.  It was not published as a release; it is
recorded as the fallback if 7.2 has to be backed out.

## Build

Branch `kernel-6.18.57`, commit 56461d5.  The kernel is pinned to v6.18.57
(tag object `c05de972…`, commit `2ff600d29ee4…`) in `versions.lock.json`,
the Buildroot defconfig and the `vendor/linux` submodule.  The patch series
applied unchanged.

Built by the `release` workflow (`workflow_dispatch`, run 39038032926,
`version=v0.2.6`) and flashed as downloaded:

```
9a4fce525316ed13be0acc81db645d0d8011d9aa19ce90557ccff61c07f0f155  m5stamp-p4-c6-linux-smp-v0.2.6-16mb.bin.gz
685180867f4d8959d84d52b19e62d83faf79766c827494640489d7cc836d23f4  m5stamp-p4-c6-linux-smp-v0.2.6-16mb.bin
3f1f79f56aa26f78c60c34de29db58273d4dba9d18b517e0747ca92d086512b8  m5stamp-p4-c6-linux-smp-v0.2.6.zip
fc99a9fb3236e1b628a69e64b4d953b12bdcafb9984b6e1dbdac11ef2926e8f4  m5stamp-p4-c6-linux-smp-v0.2.6-debug.tar.xz
```

Partitions in the zip:

```
b7d1a5d8644e2a2695b2e45d134694ab316c9275e90d5f47209bb17ad4461bd0  bootloader.bin
7937656f29960d53b63010b51836e233d092e067e320cde5778ec43e04218870  partition-table.bin
1746f4c0343686fcb9127b2ed3a80be6ca80d4edae1e586b7a08f58b635c1e2c  boot-shim.bin
790e053c6024d0c8959dd2fb8ac472ce1db46aff09a877f43294f506a75523dc  Image
838a1ed22574d27a7f2f6d98f085b66c0468bad798495f65dd9d8a5553c6510b  rootfs.squashfs
a12b18e1347ade36c499c79338ff7125bd4140bf56c64ecfedce8f93b4d3a852  esp32p4-m5stamp-p4.dtb
f3485f3b19ef885e269146900964c754c091b929d7702cec4e546ee90193330c  boot.img
```

The boot-shim prints `boot-shim git=56461d5ccc1b`; the kernel prints
`Linux version 6.18.57`; `/etc/build-info` reads `git=56461d5ccc1b`,
`dirty=0`, `version=v0.2.5-1-g56461d5`.

## Results

The same checks as the v0.2.5 record, with the same test program.

| Check | Result |
|---|---|
| `check-boot-log.py` after the flash | 11/11; `ES_AUTORUN STAGED` |
| STA settings and m5 password written into the live drive | applied without a reboot; `/etc/resolv.conf` `-rw-r--r--`; `nslookup` works |
| `main.py` + `lib/` saved to the drive | runs as `m5`; `HTTP/1.1 200 OK` |
| Edit `main.py` (v1 → v2), `autorun restart` as `m5`, `autorun=off` | restarted, restarted, stopped; `wlan0` `carrier_changes` stayed 0 |
| `timeout 2 sleep 8` | exit 143 |
| 3 resets with `main.py` on the drive | 11/11 each; `boot selection: generation=6 slot=0 crc=e7f7d8ba` each; autorun started each time |
| `dmesg` | only the `regulatory.db` load failure, as on v0.2.5 |
| Remove the test files | `config.txt` md5 back to its value before the test |

## Not covered

The same gaps as the v0.2.5 record (real USB host edits, power loss during
a flash write, a soak of hours).
