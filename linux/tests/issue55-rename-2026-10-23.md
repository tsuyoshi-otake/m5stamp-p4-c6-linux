# #55 — de-branding rename: easystick → role names (2026-10-23)

Issue #55: every user-visible `easystick` string is renamed to a role name
with no project prefix.  One-release compatibility aliases are kept in
`#55 compat` regions.  Historical records (`linux/tests/*-20XX-*.md`,
`tests/*-20XX-*`, `docs/`, `board/hardware/`, README release notes) keep
the old names.

## Name map

| old | new |
|---|---|
| `/usr/sbin/easystick-bootsync` | `bootsyncd` |
| `/usr/sbin/easystick-info` | `sysinfo` |
| `/usr/sbin/easystick-update-status` | `update-status` |
| `/usr/sbin/easystick-autorun` | `autorun` |
| `/usr/sbin/easystick-usb-lun` | `usb-lun` |
| `/usr/sbin/easystick-watchdog` | `watchdog-feed` |
| `/usr/sbin/easystick-respawn` | `respawn` |
| `/usr/sbin/easystick-mdns` | `mdnsd` (package `mdns-answer`) |
| `/usr/sbin/easystick-sys` | `run-system` |
| `/usr/sbin/easystick-boot-commit` | `boot-commit` |
| `/usr/sbin/easystick-firstboot` | `firstboot` |
| `/usr/sbin/easystick-sdio-diag` | `sdio-diag` |
| `/usr/lib/easystick/` | `/usr/lib/board/` |
| `S05easystick-tmpfs` | `S05tmpfs` |
| `S85easystick-ssh` | `S85ssh` |
| `S90easystick-watchdog` | `S90watchdog` |
| `S10easystick-config` | `S10config` |
| `default.script.d/01easystick-status` | `01status` |
| hostname `easystick` / `easystick.local` | `stamp-p4` / `stamp-p4.local` |
| `/run/easystick-status.txt` | `/run/status.txt` |
| `/etc/easystick-release` | `/etc/build-info` |
| `/etc/easystick/` | `/etc/board/` |
| `/sys/kernel/easystick/` | `/sys/kernel/board/` |
| `BR2_EXTERNAL_EASYSTICK_STAMP_P4_PATH` | `BR2_EXTERNAL_STAMP_P4_PATH` |
| `easystick-stamp-p4.dts` | `esp32p4-m5stamp-p4.dts` |
| `easystick,fifo-no-pop` | `espressif,fifo-no-pop` |
| `easystick,force-pio` | `espressif,force-pio` |
| `EASYSTICK_POOL_BESTFIT` | `NOMMU_POOL_BESTFIT` |
| `EASYSTICK_USER_RESERVE` | `NOMMU_USER_RESERVE` |
| `EASYSTICK_USER_SYSMARK` | `NOMMU_SYSTEM_MARK` |
| `EASYSTICK_SDIO_FORCE_PIO` | `SDIO_FORCE_PIO` |
| `EASYSTICK_SSH_LEDGER` | `SSH_LEDGER` |
| `EASYSTICK_CRASH_CAPSULE*` | `CRASH_CAPSULE*` |
| `EASYSTICK_WDT*` | `BOARD_WDT*` |
| `PF_EASYSTICK_SYSTEM` | `PF_SYSTEM_WORK` (value unchanged) |
| `PR_EASYSTICK_SYSTEM` | `PR_SET_SYSTEM_WORK` (value unchanged) |
| `00NN-easystick-*.patch` | `00NN-*.patch` |
| `easystick_cmd53_bb_*` | `cmd53_bb_*` |

## Compatibility (remove after v0.5.x)

- `/usr/sbin/easystick-*` symlinks to the new names
  (`board/stamp-p4/post-build.sh`)
- `mdnsd` answers `easystick.local` in addition to `<hostname>.local`
  (`package/mdns-answer/src/mdnsd.c`)
- `update-status` mirrors `/run/status.txt` to `/run/easystick-status.txt`;
  `sysinfo` falls back to the old path
- kernel patch 0076 links `/sys/kernel/easystick` → `/sys/kernel/board`

## Build check

`linux/tests/check-no-easystick.py` runs at the end of `build-m1.sh` and
fails the build if `easystick` appears outside the allowlist (dated test
records, `docs/`, `board/hardware/`, READMEs, `#55 compat` regions).

## Checks

- [x] clean build (C68 clean-release, `M3-LAB Buildroot build complete`)
- [x] `check-boot-log.py` 13/13 (captured boot, all markers pass)
- [x] host tests pass (all 10 scripts)
- [ ] SSH via `stamp-p4.local` and `easystick.local`
  (inetd `0.0.0.0:22 LISTEN` confirmed; TCP connect from host on the
  10.255.10.x path timed out — same host-side routing quirk seen in
  earlier rounds; needs a LAN run to exercise the dropbear path)
- [x] old and new command names work (`easystick-sys true` → `ALIAS_WORKS`,
  `which` resolves `sysinfo`/`easystick-info`/`run-system`)
- [x] `sysinfo` == old `easystick-info` output (identical release block)
- [ ] USB update + `config.txt` apply
- [x] STA lease (`inet 10.255.10.160/24` on wlan0)
- [ ] board-side AP
- [ ] autorun
- [ ] crash scenario, 0 faults
- [ ] 30-min soak, `alloc_fail 0`
- [x] `git grep -i easystick` outside allowlist = 0
  (build-time guard: `#55 check: no 'easystick' references outside the allowlist`)
- [x] `/sys/kernel/board/` primary + `/sys/kernel/easystick/` compat link
  both expose `nommu_pool`, `user_reserve_*`, `user_denied`,
  `system_alloc_fail`, `boot_commit`
- [x] `/run/status.txt` primary + `/run/easystick-status.txt` mirror
  both written (`# Stamp-P4 Network Status`)

## Build notes

- `0076-nommu-pool-sysfs.patch`: compat `sysfs_create_link` insertion
  required `+84`→`+87` hunk-size fix (missed count truncated the hunk
  tail: `return 0;`, `}`, `device_initcall`).
- `0090-nommu-user-reserve.patch` hunk 6 context updated to match the
  post-0076 tail (compat block sits between `sysfs_create_group` and
  `return 0`).
- `check-no-easystick.py` skips lines containing `check-no-easystick`
  so `build-m1.sh`'s own invocation does not self-flag.
- Upstream Buildroot already ships an `mdnsd` package; our package is
  `mdns-answer` (installs `/usr/sbin/mdnsd`).
