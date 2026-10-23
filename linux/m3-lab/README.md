# M3-lab: temporary SSH bring-up image

This profile is for the first assembled-board test only.  It is intentionally
less secure than the production M3 profile:

- the local account is `m5`; its password is generated per device on first
  boot and stored as `password=` in `credentials.txt` on the USB drive (#24);
- Dropbear password authentication is enabled for that account;
- host keys are generated in `/tmp` and are lost on reboot;
- Wi-Fi credentials are supplied at build time through environment variables,
  never committed to the repository.

Use it only on a private test network.  `passwd` does not persist (read-only
rootfs); edit and save `password=` in `credentials.txt` instead.
bootsyncd applies it a few seconds later and S05tmpfs at
every boot (#30).  Move to
the key-only M3 profile after the console/config overlay path
has been validated.  The lab image's `askfirst` shell is on `ttyGS1`, so the
module USB-C/COM10 path is usable during bring-up; UART0 remains the recovery
fixture.  The profile does not make USB-A a host and does not alter the
USB-A/USB-C power restriction.

## Build-time Wi-Fi provisioning

Set `WIFI_SSID` and `WIFI_PSK` in the Linux build
container before invoking `build-m1.sh --profile m3-lab`.  If either is absent,
the image still builds but reports that Wi-Fi is not provisioned and does not
start `wpa_supplicant`.

The generated Wi-Fi file is staged below the external build output and is not
copied back into Git.  Check the artifact manifest before flashing and erase
the external build directory when the test is complete.

## USB-A boot configuration ownership

The USB FAT image has exactly one owner. While it is exported through
`g_mass_storage`, Linux neither mounts nor writes the backing image.
`bootsyncd` imports the drive 3 seconds after the host's last write
(it polls the image mtime), or at once after the host's safe-eject action (the
empty legacy LUN `file` attribute is the hand-off; the LUN is reattached
afterwards). Either way it reads a copy: it validates a private read-only
snapshot and applies the configuration. The daemon never uses `forced_eject`.

Network status is written atomically to `/run/status.txt` for SSH or
serial inspection. It is not written into the exported FAT image. A validated
snapshot is staged in reserved PSRAM, and the kernel (patch 0072,
`/sys/kernel/board/boot_commit`) writes the inactive flash slot, verifies
its CRC, and atomically switches redundant metadata, as boot-shim would at the
next reboot. Power loss during the flash operation leaves the previous slot
selected. The edit is durable once the serial log shows `Saved to flash`. If
the kernel cannot write flash, bootsync reboots so that boot-shim writes it.

## SSH acceptance target (M2.5)

After the C6 SDIO slave and P4 image have been validated, the lab image should
obtain a DHCP lease (or a documented static lab address) and accept an
**external** client:

```text
[PASS] netstat shows 0.0.0.0:22 LISTEN
[PASS] Pi → DUT TCP/22 (Dropbear banner)
[PASS] Pi → DUT key-auth (or lab password once)
[PASS] ssh -T -o BatchMode=yes -o RequestTTY=no pi@<DUT> 'id'
[PASS] command stdout + exit status
[PASS] DUT still alive (no reboot during the shot)
```

Connect as `m5`.  Root logs in by key only (`dropbear -g`), and only after a
`root_authorized_keys` file is put on the drive (#37).

`S85ssh` starts inetd on `0.0.0.0:22` without waiting for `wlan0`
or `ethsta0`.

### Non-gating: DUT loopback `dbclient`

This profile enables Dropbear **client** and `tcpdump` for bring-up only.
Production `m3` disables `dbclient`. A DUT→DUT loopback SSH runs client crypto,
server crypto, inetd, and a shell together on a ~20 MB NOMMU image — that is
harsher than the product path and is **not** an M2.5 failure by itself.

`LAB_TOOLS=0` builds the same m3-lab image without the bring-up
tools: no `tcpdump`/libpcap, MicroPython (`python`, `python3`), `dbclient`,
`dropbearconvert` or `nc` (#32).  The release workflow still builds the default
(`1`), because its hashes are checked against hardware validation records; a
release switches over only after a `LAB_TOOLS=0` image has been
validated on hardware.

Prefer:

```text
PC: passive serial capture
DUT: inetd + Dropbear server only (no tcpdump, no dbclient)
Pi:  ssh -T … 'id'
```

### SSH stage ledger (pre-auth + post-auth)

The Dropbear ledger is **opt-in**. The default `m3-lab` build leaves Dropbear
unmodified so the SSH acceptance shot exercises the real command path. Set
`SSH_LEDGER=1` when a stage trace is specifically required; that
build applies
[`dropbear-patches/0001-ssh-stage-ledger.patch`](dropbear-patches/0001-ssh-stage-ledger.patch)
and its companion patches, causing each connection to emit fixed
`ES_SSH …` lines to a **dual sink** (test-only):
`/dev/kmsg` and `/dev/ttyGS1`, each opened once with
`O_WRONLY|O_CLOEXEC|O_NONBLOCK` (never stdout/stderr — that would corrupt
banner exchange under inetd). Passive WDT wipe of the printk ring still leaves
markers on the USB serial console when the tty sink is open. After one SSH
attempt:

```text
# preferred under WDT risk: passive COM10 / ttyGS1 capture
dmesg | grep ES_SSH   # only if the DUT is still up
```

Markers run `DB_ENTER → EARLY_* (commonsetup / crypto_init / load_all_hostkeys) →
RNG_* → IDENT_BEGIN/IDENT_SENT → PEER_IDENT → KEX_OK →
AUTH_REQ → PUBKEY_ENTER → AUTHKEY_* → SIG_VERIFY_* → AUTH_COMMIT → AUTH_OK →
SESSION_EXEC → VFORK_BEGIN/VFORK_CHILD/VFORK_PARENT_RESUME → EXEC_BEGIN`
then post-exec `SHELL_ENTER` / `ID_*` (BusyBox hush) → `DROPBEAR_PIPE_READ` /
`DROPBEAR_SOCKET_WRITE` (see `dropbear-patches/` + `busybox-patches/`).
(`EXEC_FAIL` only on exec failure — there is no `EXEC_OK`). Fixed tags
only; no pubkey material in logs. Exit tags: `DROPBEAR_EXIT`,
`DROPBEAR_FATAL site=N`, `SIGNAL_TERM`. `IDENT_SENT` means the userspace socket
write of the full server banner completed; Pi seeing the banner is a separate
end-to-end check. See
[`dropbear-patches/README.md`](dropbear-patches/README.md) for the KEX→AUTH
and post-exec classification tables. Rebuild with `--profile m3-lab` after
setting `SSH_LEDGER=1` when the ledger is wanted; the observer stub
that prints `SSH: deferred until L3` does not include it. The ledger is
diagnostic only and may perturb the NOMMU watchdog timing, so it is not part
of the SSH acceptance image. This change alone does not complete M2 / M2.5.

Directional bring-up also ships `tcpdump`, BusyBox `arping`/`netstat`/`nc`.
Use them only when isolating AP client isolation or hosted RX stop — not during
the minimal SSH one-shot.

The default M3-lab build keeps the functional ESP-Hosted transport patches but
applies `0024` (demote `esp_info()` in `esp_sdio.c`) and `0025` (remove the
hot-path `BOOT_CMD53_DATA` / `RXTRACE` console dumps — demoting to
`KERN_DEBUG` is not enough while the cmdline uses `loglevel=8`).  On this
NOMMU target those messages can starve SDIO and make ARP or SSH appear to fail.
`S40network` also runs `dmesg -n 1` at start so the lab shot does not need a
manual console `dmesg -n 1`.  Set `ESPHOSTED_DIAGNOSTICS=1` only when
collecting that transport evidence (it drops `0024` and `0025`); do not use
that diagnostic image for the SSH acceptance shot.

Observed 2026-08-21 on quiet M3-lab (after 0024/0025): boot console ~16 KB and
`BOOT_CMD53_DATA=0`, but host→DUT ARP/ICMP still failed until `dmesg -n 1`.
With that applied, ping and `SSH-2.0-dropbear_2026.91` / password `CONNECT_OK`
succeed in ~0.5 s.  Any `exec_command` (including `/bin/true` or `id`) then
wedged networking within seconds and a concurrent ttyGS1 capture showed
`rst:0x7 (HP_SYS_HP_WDT_RESET)` — so M2.5 `ssh … 'id'` remains blocked on the
post-auth spawn/WDT path, not on banner reachability.

Ledger refinement (same day, `SSH_LEDGER=1`):

- Auth-only idle SSH (~35 s, no channel/exec) **survives** through `AUTH_OK`
  and clean `DROPBEAR_EXIT`.
- `/bin/true`: last mark is `CHANNEL_EOF` → `ENCRYPT_DONE` (no
  `EXIT_STATUS` / `CHANNEL_CLOSE`); then `HP_SYS_HP_WDT_RESET`.
- `id`: reaches `CHANNEL_DATA` `SOCKET_WRITE_DONE` (and sometimes also
  `CHANNEL_EOF` `ENCRYPT_DONE`); host still gets empty stdout; then WDT.
- Raising MWDT `timeout-sec` 30→120 does **not** clear M2.5. A flash-only
  DTB with `watchdog@500c2000` `status = "disabled"` still left the DUT
  unresponsive on ttyGS1 after the same exec hang (hard lock / IRQ-off
  class, not “need a few more seconds”).
- Next classify with host TX stage ledger `0023-A` (`ES_TX` …
  `CMD53_OK/ERR` on TCP sport 22) rather than further WDT stretching.

TX-ledger classification (2026-08-21, `ESPHOSTED_TX_LEDGER=1` on
quiet+SSH-ledger rootfs; capture
`console-txledger-true-then-id-20260821.bin`):

- Pre-exec sport-22 frames (IDENT / KEX / AUTH_OK / CHANNEL_OPEN_CONFIRM)
  complete `NETDEV_XMIT → ENQUEUE_OK → DEQUEUE → CREDIT_OK → CMD53_ATTEMPT →
  CMD53_OK`.
- `/bin/true` after `VFORK_PARENT_RESUME`: first post-exec frame reaches
  `CMD53_ATTEMPT` and **never** emits `CMD53_OK` or `CMD53_ERR` (counts
  ATTEMPT=10 / OK=9 / ERR=0 in that shot). No `CREDIT_NO_BUFFER`.
- Dropbear still prints `CHANNEL_EOF` → `ENCRYPT_DONE` after that stuck
  `CMD53_ATTEMPT`, but no further `ES_TX NETDEV_XMIT` for the EOF wire
  frame; host sees `ConnectionReset` and DUT ping dies.
- Class: hang inside / around `esp_write_block` (CMD53) on the post-exec
  TX, not enqueue/credit starvation and not a missing netdev xmit.

Experiment A (2026-08-21, known-good squashfs unsquash/LZ4 repack):
noop CONNECT_OK PASS; all-CMD53 split markers then reached `CMD53_OK`
(instrumentation-sensitive vs ledger). Sport22+`trace_id` follow-up
(`console-a-sport22-true-20260821.bin`): same `trace=10` entered and
**returned** from `sdio_memcpy_toio()` with `-ETIMEDOUT` (`MEMCPY_DONE
ret=-110` means the C function returned `-ETIMEDOUT`, not that the
transfer completed). That rules out “call never returns / confirmed
memcpy stall” for this instrumented shot; it does **not** yet split CMD53
command-response timeout (RTO) from data-phase timeout (DRTO/EBE), nor prove
the timeout causes DUT death. Marker density still perturbs behavior.
FORCE_PIO deferred until error provenance is classified. Evidence:
`REPORT-A-split.md`, `REPORT-A-sport22-trace.md` under
`D:\Users\Developer\tmp-20260820\p4-m3-lab-quiet-txledger-20260821\`.

Experiment B (2026-08-21): error-only DW-MMC CMD53 provenance
(`Image-b-errprov` / `0051`). Zero CLAIM/MEMCPY; `ES_MMC CMD53_ERR` count 0;
post-VFORK stops at `CMD53_ATTEMPT` (restores light-ledger
ATTEMPT-without-observable-return). Together with A this is strong
**instrumentation-sensitive** evidence; hot-path printk density is the
leading perturbation, but Image layout/timing from `0051` is not yet
excluded. No RTO/DRTO split → FORCE_PIO still deferred. Binary attestation:
`m3-lab/ATTEST-0051-binary.md`. Shot report: `REPORT-B-errprov.md` on the
staging tree.

Experiment C (0052, pre-C rework): retention black-box. Wrapper forces
`FORCE_PIO=0` and clears IDMAC/ledger ambient flags; maps `m3-lab ->`
boot-shim m2; always `linux-dirclean`; writes fail-closed
`cmd53-bb-final-shot-manifest.json` (`shot_c_allowed` only for
non-selftest). CMD53-only attribution on cmd_done/err/cto. See
`cmd53-bb/README.md`. Sparse IDF cannot complete HW.

Opt-in: `ESPHOSTED_TX_LEDGER=1` applies
`0023-sdio-tx-stage-ledger.patch` (includes sport22-only
CLAIM/MEMCPY with caller-owned `trace_id`). Default quiet acceptance
images omit it. `DW_MMC_CMD53_ERR_PROV=1` applies `0051`
error-only DW-MMC provenance (default off). `CMD53_RETENTION_BB=1`
requires `build-cmd53-bb.sh` (mutually exclusive with `0051`).

This is a bring-up check, not a release credential.  Record the lease and
serial evidence outside Git, then rebuild with the production M3 key-only
profile before any wider network test.
