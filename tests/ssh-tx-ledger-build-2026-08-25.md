# SSH TX ledger observer build — 2026-08-25

Status: **BUILT — P4-ONLY FLASHED FOR OBSERVATION**

This is an observation-only image for the existing SSH disconnect analysis.
It is not an A/B behavior comparison. The C6 image was unchanged and no C6
write was performed. A later, separately gated P4-only flash and capture are
recorded in
[`ssh-tx-ledger-capture-2026-08-25.md`](ssh-tx-ledger-capture-2026-08-25.md).

## Fixed build conditions

- Profile: `m3-lab`
- `EASYSTICK_SDIO_FORCE_PIO=0`
- `EASYSTICK_IDMAC_NONCOHERENT_RING=1`
- `EASYSTICK_ESPHOSTED_DISABLE_0010=1`
- `EASYSTICK_ESPHOSTED_TX_LEDGER=1`
- `EASYSTICK_ESPHOSTED_DIAGNOSTICS=0`
- `EASYSTICK_SSH_LEDGER=0`
- `EASYSTICK_DW_MMC_CMD53_ERR_PROV=0`
- `EASYSTICK_CMD53_RETENTION_BB=0`

The generated profile stamp and kernel configuration passed checks for the
DMA/noncoherent settings, RV32 NOMMU, `CONFIG_FRAME_POINTER=y`,
`CONFIG_STACKTRACE` disabled, and `CONFIG_KALLSYMS` disabled.

## Patch-series correction

The first build exposed that `0023-easystick-sdio-tx-stage-ledger.patch`
contained duplicate quiet-diagnostics and hexdump hunks owned by `0024` and
`0025`. Those non-ledger hunks were removed from `0023`; a clean temporary
source copy then accepted the complete selected series in order:
`0001`–`0009`, `0011`–`0013`, `0015`–`0025`.

The built `esp32_sdio.ko` contains `ES_TX`, `CMD53_MEMCPY_ENTER`, and
`CMD53_OK` markers. The ledger patch changes logging only; it does not change
C6 firmware, credit/retry policy, IRQ/DMA configuration, Dropbear, BusyBox,
WDT, or PSRAM.

## Artifact evidence

Artifacts are retained outside Git at:

`D:\Users\Developer\easystick-tmp-20260820\easystick-p4-m3-lab-dma-nc-asus-20260824\ssh-tx-ledger-2026-08-25\`

| Artifact | Bytes | SHA-256 |
| --- | ---: | --- |
| `bootloader.bin` | 22,976 | `0d74985a383a0509376df9a065c2efe63a6df06d6d1bd43760214e380b3fcc40` |
| `partition-table.bin` | 3,072 | `d076fd66f0f4bd3f9f423761ef10b73652f2359f190c5ffef0164f657c40d9d4` |
| `boot-shim.bin` | 212,224 | `edef63700eda9df4b903674f4d5b1862f437ae850b6dd91328ddc8ba65e1abbc` |
| `Image` | 6,576,968 | `ca528859796f1f01b724c685d702dcdcc812c393ba8d6ffcfa57b98f252f3009` |
| `rootfs.squashfs` | 2,732,032 | `97e2354c17b156b5fcdef9dd77bcb58b611c7dd74832381543caf19175c044bf` |
| `easystick-stamp-p4.dtb` | 3,372 | `7c3bd1e042d71aef80ebbeb37cfb402cabb400e3a41dc82b4a4b5d59a18eb163` |
| `vmlinux` | 11,649,652 | `ba5e878e895bd99cc95644d4032dce53b368d8a301e842e5308c7bf475947b41` |
| `System.map` | 1,055,083 | `ef29eb5b188228aaa01e8af120128bb6b630ff7ec6fb9ae066ffb9e60f4de85e` |
| `linux.config.generated` | 58,081 | `9145936e9409b72962ef0373f0b8a73b4f954b5ac438561dbdebedf2fa68ce62` |

`SHA256SUMS.txt` was checked with `sha256sum -c` and all entries passed.
The M2 candidate map verifier also passed with status
`candidate_not_for_flash`, 11 regions, and the three generated flash
artifacts within their regions.

## Rootfs checks

The final SquashFS was extracted and verified to contain:

- `etc/init.d/S40network`
- `etc/init.d/S85easystick-ssh`
- `etc/easystick/wpa_supplicant.conf`

The Wi-Fi configuration matched the supplied build environment. The resulting
SquashFS mode for `wpa_supplicant.conf` was observed as `0644`, although the
provisioning staging file was `0600`; this observer build did not change that
separate permission issue.

## Flash gate and follow-up

The build-time gate was closed without a write. On 2026-08-25, the preserved
stock readback was revalidated and the separately gated
`flash-candidate.ps1 -AllowCandidateWrite` operation wrote and verified this
image to P4 `COM10` only. The subsequent UART capture and one controlled SSH
reproduction are recorded in
[`ssh-tx-ledger-capture-2026-08-25.md`](ssh-tx-ledger-capture-2026-08-25.md).
