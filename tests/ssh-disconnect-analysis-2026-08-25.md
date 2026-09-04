# SSH切断原因分析 — 2026-08-25

Status: **ANALYSIS — current M3-lab DMA candidate plus TX-ledger follow-up; no A/B change**

## Scope and fixed inputs

- Target: EasyStick Stamp-P4, P4 `COM10`, `10.255.10.161`.
- C6: unchanged; no C6 write was performed.
- Flash: the baseline capture below used the already-flashed DMA candidate and
  only reset P4. A later, separately gated P4-only observer flash is recorded
  in the follow-up report linked below.
- Transport: the already-flashed DMA noncoherent-ring candidate was used
  unchanged.
- Candidate artifact hashes:

| Artifact | Bytes | SHA-256 |
| --- | ---: | --- |
| `Image` | 6,576,968 | `2fa6a8c9f8701903f1cf78cd37389687d61247a8dc04af444ff33522c2ca9194` |
| `rootfs.squashfs` | 2,727,936 | `b71bb646032adc7f2f388e2bc18cc96a87610b01436cc6506de689f6cf3493ba` |
| `easystick-stamp-p4.dtb` | 3,372 | `7c3bd1e042d71aef80ebbeb37cfb402cabb400e3a41dc82b4a4b5d59a18eb163` |
| matching `vmlinux` | 11,649,652 | `bf91639eaf3b2e040e4fb890e92200aecf83d9ccc5b3d011aee373732b13947b` |
| matching `System.map` | 1,055,083 | `ef29eb5b188228aaa01e8af120128bb6b630ff7ec6fb9ae066ffb9e60f4de85e` |
| generated Linux config | 58,081 | `9145936e9409b72962ef0373f0b8a73b4f954b5ac438561dbdebedf2fa68ce62` |

The matching symbol artifacts are retained outside Git at:

`D:\Users\Developer\easystick-tmp-20260820\easystick-p4-m3-lab-dma-nc-asus-20260824\ssh-disconnect-analysis-2026-08-25\`

The generated configuration has `CONFIG_FRAME_POINTER=y`, but
`CONFIG_STACKTRACE` and `CONFIG_KALLSYMS` are disabled. The staged kernel
patches include the normal WDT and SDIO/DMA diagnostics, but not the 0054
crash-capsule patch.

## One controlled reproduction

UART was captured with
`capture-boot.ps1 -Reset` before the SSH operation. The SSH client opened one
interactive channel, received the server shell bytes, sent **no remote
command**, and then closed the channel.

Host-side timeline, retained outside Git:

`D:\Users\Developer\easystick-tmp-20260820\easystick-p4-m3-lab-dma-nc-asus-20260824\ssh-disconnect-analysis-2026-08-25\ssh-channel-repro-2026-08-25.json`

JSON bytes: 1,048  
JSON SHA-256: `a1d6caa018465a8579e5aa186a1bf18ff6563d0bd1c83f6cf65b051c3206712c`

Observed:

```text
BEFORE_TCP22=True
SSH_AUTHENTICATED elapsed=0.640
CHANNEL_OPEN elapsed=0.719
CHANNEL_RX bytes=258 elapsed=0.922
CHANNEL_STATE_AFTER_5S active=1 closed=False
AFTER_CLOSE_TCP22=False
AFTER_CLOSE_PING=False
```

The raw UART capture is:

```text
D:\Users\Developer\easystick-tmp-20260820\easystick-p4-m3-lab-dma-nc-asus-20260824\uart-ssh-disconnect-analysis-2026-08-25.bin
bytes 15549
SHA-256 dd7c1c08b05551fc65aaadd45a3a2bce8b1c5985a66bd2054aba023c66981ac6
```

The capture includes:

```text
rst:0x7 (HP_SYS_HP_WDT_RESET),boot:0xc (SPI_FAST_FLASH_BOOT)
ES_SDIO_LOAD_OK source=S40 requested_mhz=5 module_param_mhz=5 ...
M3-lab: association complete; using static IPv4
ES_HOSTKEY INETD_START
M3-lab: starting password-enabled Dropbear on TCP/22
```

No Linux `panic`, `oops`, or decoded stack trace was emitted. The
`Core1 Saved PC:0x4ff02c82` value is outside the Linux `vmlinux` symbol range
(`0x00000000..0x00675348` in this relocatable image), so it cannot be
symbolized with the matching Linux ELF. It is a ROM/boot-shim reset breadcrumb,
not a Linux call-site address.

The UART collector reported its first byte at 138.864 seconds, so the WDT
reset is strongly correlated with the failed SSH-channel run but is not
timestamped at byte-level against the host event in this shot. That limitation
is retained rather than presenting the correlation as an exact causal trace.

## Analysis result

The failure is not in Wi-Fi association, TCP/22 listening, SSH banner exchange,
or password authentication. The first observed failure is after channel
creation and server-to-client shell data, followed by loss of both TCP/22 and
ICMP. A subsequent P4 boot reports `HP_SYS_HP_WDT_RESET`.

Current evidence therefore classifies the defect as:

**post-auth SSH channel traffic causes a P4-side network/SDIO-DMA hard wedge,
which is eventually recovered by the MWDT.**

The exact blocked instruction is not proven by this shot. The repository's
earlier TX-ledger analysis in
[`linux/m3-lab/README.md`](../linux/m3-lab/README.md) reports the same
symptom with the first post-exec frame stopping at `CMD53_ATTEMPT`, around
`esp_write_block`/`sdio_memcpy_toio`, but its referenced raw captures are not
present in the current external evidence directory. That historical result is
corroboration, not a substitute for a same-image trace.

## Observation-only follow-up design

If the exact failing stage is required, use one diagnostic image with the
current DMA/noncoherent configuration unchanged and only the existing
host-TX stage ledger enabled:

- `EASYSTICK_SDIO_FORCE_PIO=0`
- `EASYSTICK_IDMAC_NONCOHERENT_RING=1`
- `EASYSTICK_ESPHOSTED_DISABLE_0010=1`
- `EASYSTICK_ESPHOSTED_TX_LEDGER=1`
- keep the quiet 0024/0025 console behavior
- do not enable the Dropbear hot-path ledger in the first pass
- no C6 image or C6 write

The ledger in
[`0023-easystick-sdio-tx-stage-ledger.README.md`](../linux/buildroot-external/package/esp-hosted-ng/0023-easystick-sdio-tx-stage-ledger.README.md)
is observe-only and distinguishes `NETDEV_XMIT`, enqueue/dequeue, credit,
`CMD53_ATTEMPT`, `CMD53_MEMCPY_ENTER`, `CMD53_MEMCPY_DONE`, and
`CMD53_OK/ERR`. This is an analysis image, not an A/B behavior comparison.

If the device resets before the ledger returns, the next diagnostic image
should additionally use the existing 0054 WDT pretimeout/crash-capsule path
with stack tracing and debug symbols enabled. That capsule records registers
and only 32 bytes of stack; it is not a full RAM dump. At the design stage,
build, rootfs verification, and P4-only flashing required a separate explicit
write approval; that follow-up was later performed and is recorded below.

## Observer build result

The planned TX-ledger image was built and verified without writing either
device. Its fixed flags, patch-series correction, artifact hashes, rootfs
checks, and `candidate_not_for_flash` result are recorded in
[`ssh-tx-ledger-build-2026-08-25.md`](ssh-tx-ledger-build-2026-08-25.md).

## Observer flash and TX-ledger result

After the build report was frozen, the preserved stock readback was revalidated
and the observer image was flashed and verified on P4 `COM10` only. A passive
UART capture plus one authenticated SSH-channel reproduction then placed the
first failing boundary between `DEQUEUE` and the credit/token-register check,
before `CMD53_ATTEMPT`. The raw capture hashes, control captures, host timeline,
stage sequence, and evidence limits are recorded in
[`ssh-tx-ledger-capture-2026-08-25.md`](ssh-tx-ledger-capture-2026-08-25.md).
