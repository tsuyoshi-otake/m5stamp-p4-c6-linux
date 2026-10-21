# #39 / #40 hardware checks (2026-10-19)

Board: M5Stamp ESP32-P4 + C6 (SDIO), P4 USB Serial/JTAG on COM10.  Only the
P4 flash was written; the C6 was not touched.  Local builds from branch
`issue39-40` with `build-c68.sh --profile m3-lab`, flashed with
`flash-candidate.ps1 -PreserveBoot` (-PreserveBoot was the opt-in then; since 2026-10-21 R2 preserving the boot slots is the default and the switch is a no-op; -WriteBoot writes the boot FS) (the drive and credentials stayed).  No
SSID, PSK, password or client address is quoted here.

#39: a config.txt save applies only the keys that changed (`S40network apply
KEY...`) instead of restarting the whole network.  #40: the NOMMU pool is
visible in `/sys/kernel/easystick/nommu_pool`, and the network scripts fork
less during a reload.

## Builds

| Build | Commit | Notes |
|---|---|---|
| b | 77e1189 | first build with the review fixes |
| c | c8225d6 | modprobe helper emptied with `echo` |
| d | efd35a5 | deleted `hostname=` falls back to the boot name |
| e | 35f2acb | `country` live apply (a443267), DHCP after a PSK fix by HUP |

The kernel Image (`3a8e9446…`) and the DTB (`a12b18e1…`, as in v0.3.1/v0.3.2)
are the same in builds b to e.  Build e partitions:

```
27ce052bb6f4176c938f8a2617e9c269b4a9fd6b5a7ae94b7b17839447467ff7  bootloader.bin
7937656f29960d53b63010b51836e233d092e067e320cde5778ec43e04218870  partition-table.bin
fd3db5d2b8b4704604deea801be172ae78afbc9d20153ff9ce77bdce104be750  boot-shim.bin
3a8e9446adf621db5dba05182c9503a25ad6961f4e39dac0de9fb8ff67499cf6  Image
97eafd8c35766df416522df162303f8b506b7b6167784b69d19dad4e0acd93ec  rootfs.squashfs
a12b18e1347ade36c499c79338ff7125bd4140bf56c64ecfedce8f93b4d3a852  easystick-stamp-p4.dtb
ac7a60875eabe0d0b6d023afcbd35f7d35181b2961f48e9108bf6f05b05f04be  boot.img
```

`check-boot-log.py` passed 11/11 after flashing each of builds b, c, d and e.

## #40: NOMMU pool and process count

| Check | Result |
|---|---|
| `/sys/kernel/easystick/nommu_pool` | total 10485760, used about 5.1–5.7 MB at idle, largest_free 4194304, alloc_fail 0, fallback 0, 2 regions (4 MiB + 6 MiB) |
| `/run/easystick-status.txt` | carries the `pool_*` fields |
| `/proc/sys/kernel/modprobe` | empty (builds c to e; build b still had the path, fixed in c8225d6) |
| Peak user processes during a full network restart (niced 30 ms sampler) | 15, was 17 before.  The DHCP hook chain, `cat` and `sleep` no longer appear.  The 12-process target of #40 is **not** met for a full restart |

The sampler itself runs at 100% CPU; left running for about 100 s it lets the
watchdog reset the board even with no reload in progress, so it was only run
niced and briefly.  Those resets are the sampler's, not this change's.

## #39: per-key apply

Each change was made by editing config.txt on the live drive from the serial
console; bootsync saved it to flash and called `S40network apply` with the
changed keys.  An SSH session was held open and the board was pinged once a
second during each apply.

| Change | Console | wpa_supplicant | Ping / SSH |
|---|---|---|---|
| `ssh_password_auth` | `ES_CONFIG: ssh_password_auth= changed; applies at the next boot` | not restarted | 1–3 of 45–50 lost, scattered (8 in a row before); SSH stayed |
| `hostname` | none (applied with `hostname`) | same PID | the new name resolved over mDNS; SSH stayed |
| `hostname` deleted | back to the boot name (build d) | same PID | — |
| `country` | `[bootsync] Configuration changed: country`; `iw reg get` 00 → JP (build e; on build d it stayed 00, fixed in a443267) | same PID | SSH stayed |
| `wifi_pmf` (HUP path) | `Wi-Fi settings applied without a restart (wpa_supplicant reloaded)` | same PID | 8 of 75 lost within about 14 s — about the same outage as a full restart; SSH stayed |
| unknown key | `has no targeted apply; restarting the network` | restarted | 7 in a row lost; SSH stayed |
| two saves in quick succession | coalesced into one batch (`hostname zz_unknown`) | — | no lock left behind |

`wifi_pmf` only accepts `1`; another value is ignored and the conf is
regenerated unchanged, so that check exercised the reload path, not a real
PMF change.

### Wrong PSK, then the correct PSK again

| Step | Result |
|---|---|
| `wifi_psk` set to a wrong value | `Wi-Fi reload without a restart failed; restarting the network`; after the restart `operstate` stays `dormant`, status `AUTH_FAILED`, no `udhcpc` |
| `wifi_psk` restored (build d) | `Wi-Fi settings applied without a restart`, `operstate` up, but **no IPv4 address** and status still `AUTH_FAILED`: `start()` had never reached DHCP, so the HUP path's USR2/USR1 renew went to no client.  The board was off the network until the next restart |
| same, build e (35f2acb) | `Wi-Fi settings applied without a restart`, address obtained, status `CONNECTED`, one `udhcpc`, ping 4/4 |

Before #39 every save took the full restart, which always ran DHCP, so the
build d behaviour was a regression of this change; 35f2acb starts DHCP when
no client is running.

### Root access (#37) after the change

| Check | Result |
|---|---|
| `su` with a wrong / the right password | logged `-` / `+` |
| root key: install, login, revoke | OK |
| key install with `/tmp` full (0 blocks free) | `ES_SSH_KEYS FAIL: cannot write /root/.ssh/authorized_keys (is /tmp full?)`, root login refused; after freeing space and saving again the key installed |

The drive's config.txt was put back to its original contents after the tests
(same md5 as before), and no key file was left on the drive.

## Not tested

- Switching to a different AP (only one AP was available), `wifi_mode`
  between STA and SoftAP, `channel`, `ntp_server` and `debug`.
- The `cryptpw` / ENOMEM fail-safe of the credentials swap.
- A pipeline or SSH transfer running during a reload, and a 30-minute soak.
