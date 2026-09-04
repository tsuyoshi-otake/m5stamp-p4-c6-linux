# SSH wedge / stacktrace crash-capsule validation

Date: 2026-08-30 JST  
Evidence root: `C:\Users\developer\tmp\p4-ssh-wedge-stacktrace-20260829`  
DUT: P1-NG; Zephyr before candidate flash, Linux candidate after flash

## Result

- Image/build contract: **PASS**.
- Candidate flash and read-back verification: **PASS**.
- Wi-Fi association and target reachability before the trigger: **PASS**.
- Signature-matched SSH wedge: **PASS** on both counted shots.
- Runtime crash capsule populated: **FAIL**.
- Runtime stacktrace usable: **FAIL**.

The built image contains the watchdog pretimeout/capsule code and `dump_stack()`, but the real SSH wedge did not produce a pretimeout line, capsule commit, or call trace. Both counted shots reset with `HP_SYS_HP_WDT_RESET`; the boot shim reported `CRASH_CAPSULE empty/invalid`.

## Build provenance and contract

The r2 build used the existing resume tree `...\build-rerun`, the existing Docker volumes `easystick-p4-wdt-0054-capsule-build-fast-20260825`, `easystick-p4-wdt-0054-capsule-ccache-20260825`, and `easystick-p4-wdt-0054-capsule-dl-20260825`, and a direct serial `make -j1 ... all`. The final container state was `exited|0|false` (exit code, OOMKilled). The failed first tree `...\build` was not used by r2.

Source provenance:

- Main repository HEAD: `ea7c34e609b261a0a8b58242ddf7184d8fa1b1b6`.
- Linux submodule HEAD: `acb7cf4c1184e27622be0faf89244d5001ed1e87`.
- The Linux submodule had 13 pre-existing dirty paths. Its pre-existing zero-byte `.git\modules\...\vendor\linux\index.lock` was retained; it was not deleted, and the dirty tree was not forcibly restored.
- Rendered patch `0054` SHA-256: `bc3d81cfc55241c4518db4de6bf45397bcd1dd6d1c8d8193861631c14911c646`.
- Attestation source SHA-256: `ef05e1e4871c811dc36e7fcfa1cb42eed9a640decc51f726ddd51d224face83b`.
- `vmlinux` SHA-256: `103128bcb224eaedd7f92c06578ec62326190bc80661ea79417b1cabab1c324e`.
- The pretimeout-order gate passed with stacktrace enabled and a 30,000 ms grace-cap expectation. The resumed direct `make` bypassed the normal post-make attestation hook; the same source/order gates were run afterward and passed.

The image has `CONFIG_ESP32P4_WDT=y`, `CONFIG_STACKTRACE=y`, `CONFIG_KALLSYMS=y`, `CONFIG_KALLSYMS_ALL=y`, `CONFIG_DEBUG_INFO=y`, DWARF4, and frame pointers. DTS watchdog timeout is 120 seconds. The attestation contains the expected watchdog markers, `easystick_capture_crash`, `dump_stack();`, and symbols `esp32p4_wdt_arm`, `esp32p4_wdt_pretimeout`, and `easystick_capture_crash`.

The final-shot manifest was verified immediately before flashing: `shot_c_allowed=true`, product `m3-lab`, boot shim `m2`, CMD53 capsule PA `0x50108080u`, BB version 6, size 288 bytes, CMD52 marker enabled, and the intended noncoherent IDMAC-ring contract enabled. Its copied metadata and all selected evidence are covered by `SHA256SUMS.txt`.

## Flash

The six candidate files were copied to `flash-artifacts` and flashed with:

```text
pwsh flash-candidate.ps1 -AllowCandidateWrite
```

The gate passed on COM10 with stock SHA-256 `229459f251eaf6222f0c07968702d72a3818e520da1788ce465027d969020c24`. The C6 write was not performed. All six P4 writes reported verified hashes, and partition-table, boot shim, Image, rootfs, and DTB read-back verification passed. The command exited 0. A pinned esptool hard reset was then used only to boot the flashed candidate; it performed no write. Flash and reset logs are in the evidence root.

## Reproduction method

The orchestrator opened COM10 at 115200 once, held DTR/RTS inactive, did not reopen the port, and used sidecar liveness/port probes that never opened COM10. It required a verified ICMP reply from `10.255.10.161` before starting SSH. The background beacon was:

```sh
while :; do read u _ < /proc/uptime; echo ESLIVE $u; sleep 1; done &
```

Shot 2 reached the target at ICMP attempt 7 (TTL match, 52 ms), after association and static IPv4 setup. Relevant console lines included:

```text
M3-lab: association complete; using static IPv4
M3-lab: static IPv4 fallback complete
inet addr:10.255.10.161  Bcast:10.255.10.255  Mask:255.255.255.0
M3-lab: starting password-enabled Dropbear on TCP/22
```

No Wi-Fi credential was copied into the evidence set.

The SSH probe authenticated and sent `id`, then wedged without output:

| Event | Shot 2 UTC / elapsed |
|---|---:|
| SSH start | `2026-08-30T06:27:22.627Z` / 0.000 s |
| TCP/22 before | 0.141 s, reachable |
| Authentication | 0.610 s |
| Channel open | 0.625 s |
| `EXEC_SENT id` | 0.688 s |
| Exit timeout | 12.703 s, stdout/stderr 0, channel still open |
| TCP/22 after | 18.719 s, unreachable |

The harness exit code 0 means the harness completed its observation window; it does not mean `id` returned successfully. Shot 1 showed the same sequence: authentication at 0.578 s, `EXEC_SENT` at 0.625 s, zero-output timeout at 12.656 s, and TCP/22 unreachable at 18.687 s.

## UART/reset evidence

Shot 2 captured 27,622 bytes from the single COM10 open; capture SHA-256 is `0584ab3cf2e261601a38549ff7c64121d17a8aa94b7be578c64e1663716eefaa`. The analyzer found four `ESLIVE` lines, uptime 22.69 to 25.76 seconds, span 3.07 seconds, monotonic cadence 1.02–1.03 seconds, then:

```text
ESP-ROM:esp32p4-eco2-20240710
rst:0x7 (HP_SYS_HP_WDT_RESET),boot:0xc (SPI_FAST_FLASH_BOOT)
...
easystick-boot: CRASH_CAPSULE empty/invalid
```

The last beacon was immediately followed by the reset in the decoded UART stream (`lines_between_last_beacon_tick_and_reset=[]`; verdict `RESET_DURING_CAPTURE`). No `EASYSTICK_WDT PRETIMEOUT`, `EASYSTICK_WDT CAPSULE_COMMIT`, or `Call Trace:` occurred in the shot. After reset the board booted Linux again and re-associated Wi-Fi.

Shot 1 was also a single-open capture, 27,573 bytes, SHA-256 `3e5a6930af89fe45b52b4a0c8e0bdd09f1dc61b446c8aed5e9cdae6c8a67d97a`, with the same reset/capsule outcome. The earlier pre-reset ROM-bootloader attempt is preserved as an unreachable control record and is not counted as a reproduction shot.

The UART stream has no independent per-line timestamp. Therefore the analyzer's UTC mapping of the beacon is explicitly an extrapolation; this report uses the measured host SSH ordering and the UART reset ordering, and does not claim an exact `EXEC_SENT`-to-reset delay.

Because the capsule is empty, no runtime PC/RA/SP, subsystem fields, or usable stack are available; `addr2line` cannot be meaningfully applied. The statement that the wedge may be entering a different or earlier HP-system watchdog path is only a hypothesis, not an established mechanism result.

## Evidence and cleanup

Primary evidence is under `C:\Users\developer\tmp\p4-ssh-wedge-stacktrace-20260829`; `SHA256SUMS.txt` covers the flash artifacts, build/flash/reset/orchestrator logs, raw UART captures and metadata, analyses, attestation, and final-shot metadata. The large `build-rerun` tree and the unused empty failed `build` tree are disposable only after the completed build and evidence checks; no active process references r2, and the r2 container is stopped successfully. Named Docker volumes were preserved. No Docker prune, volume removal, repository push, or Issue comment was performed.
