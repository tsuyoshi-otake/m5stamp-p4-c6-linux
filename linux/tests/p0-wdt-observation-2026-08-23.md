# P0 WDT observation evidence — 2026-08-23

Status: **PASS** for the bounded UP-profile WDT positive control. This closes
the P0 observation gate only; it does not accept Linux SDIO, SSH, NTP, SMP, or
the C6 image.

## Test boundary

- Target: EasyStick Stamp-P4, ESP32-P4 revision v1.3
- P4 port: `COM10`, MAC `e8:f6:0a:e2:5e:73`
- C6: unchanged and not written
- Profile: UP / `m3-lab`, no `--smp`
- WDT test parameters: `test_timeout_s=5`, `test_no_feed=1`
- Retention PA: `0x50108080u`
- Tool: esptool 4.8.1
- Recovery image:
  `C:\Users\developer\tmp\easystick-p4-stock-20260809-esptool481-full-v2.bin`
- Recovery image SHA-256:
  `229459f251eaf6222f0c07968702d72a3818e520da1788ce465027d969020c24`

## A1 — feed-stop trigger only

A1 used the WDT crash-capsule implementation with the feed suppression
parameter and no live UART markers. The Linux-only build exited with code 0;
its attestation gate passed and its Image SHA-256 was
`4f8ff7d80d227d7cd28dbb668ee2dd18e1eb144b4765cd355df01429bb5c1163`.

Raw capture, retained outside Git:

```text
D:\Users\Developer\easystick-tmp-20260820\easystick-p4-wdt-0054-up-20260822-r2\p0-wdt-positive-a1-20260823\uart-a1-after-hard-reset.bin
bytes 66944
SHA-256 ec0d23ed8c12a753d7342001431ac9382752f45fc82e229d143c7d711a832273
```

The capture contained `rst:0x7` and subsequent
`CRASH_CAPSULE VALID ... reason=1` / `FLASH_OK` records, while
`PRETIMEOUT` and `CAPSULE_COMMIT` were absent. This passed the feed-stop and
persistent-capsule portion of the A/B, but left live pretimeout visibility
unproven.

## A2 — live marker visibility

The A2 functional delta was limited to two `pr_emerg` calls in the 0054
watchdog source:

1. `EASYSTICK_WDT PRETIMEOUT` at the pretimeout IRQ entry.
2. `EASYSTICK_WDT CAPSULE_COMMIT` immediately after the commit word is written.

WDT register programming, feed suppression, capsule layout, boot-shim, PA,
0052, and 0053 were unchanged. The canonical generator reported
`0054_CANONICAL_OK`. The A2 rendered 0054 SHA-256 was
`3d8cd4dd6c8331810773d484976f27519620b765160a5ae1392dfe29f0663c5e`.

The A2 build exited with code 0. Its fail-closed attestation reported:

- source SHA-256:
  `1196c3aa6d96dad58ec462995dd2eb1eb6f1185965705cf2d54d426b06e82586`
- `vmlinux` SHA-256:
  `c9155a7812147d179bc9561019867828e74c4c16c72740c249af034a1e7d0e95`
- `Image` SHA-256:
  `ee7c151587b26284df01b9f51f0fca144d484b0392e1c9b0042ededb9129e03a`
- required source markers, symbols, and disassembly checks: PASS

The A2 flash set preserved the A1 static artifacts:

| Offset | Artifact | Size | SHA-256 |
| ---: | --- | ---: | --- |
| `0x2000` | `bootloader.bin` | 22,976 | `65fc14fba12edeec7151b1d2d5b1f97f08f84662dcbb5947899290390c8efced` |
| `0x8000` | `partition-table.bin` | 3,072 | `d076fd66f0f4bd3f9f423761ef10b73652f2359f190c5ffef0164f657c40d9d4` |
| `0x10000` | `boot-shim.bin` | 218,400 | `4321897f157f4783b8fd9d72561daddf6e319513182dabeacab0f6926ecd1742` |
| `0x90000` | `Image` | 6,577,024 | `ee7c151587b26284df01b9f51f0fca144d484b0392e1c9b0042ededb9129e03a` |
| `0x810000` | `rootfs.squashfs` | 2,727,936 | `fe2475540071768cc7abd4f3a488f6c031339df9bfd955007b4a12e9cdf8bc9b` |
| `0xf10000` | `easystick-stamp-p4.dtb` | 3,428 | `c2c8a7eea0923030b202721ff3103637c6051a94ba3580e4df74534ea2d53609` |

The P4-only write and verify gate passed. Raw capture, retained outside Git:

```text
D:\Users\Developer\easystick-tmp-20260820\easystick-p4-wdt-0054-up-20260822-r2\p0-wdt-positive-a2-20260823\uart-a2-after-hard-reset.bin
bytes 67414
SHA-256 4fd887a432cdff3e9b83de3a66495f73f0daffadaf041536fd294cc63fe92198
capture JSON SHA-256 412bac1b2c057b8af1165c92750fe8d6779397fe4344c0db5d5f9ca958a06398
```

Observed counts in that capture:

- `EASYSTICK_WDT PRETIMEOUT`: 6
- `EASYSTICK_WDT CAPSULE_COMMIT`: 6
- `rst:0x7`: 4
- `CRASH_CAPSULE VALID`: 3
- `CRASH_CAPSULE FLASH_OK`: 3
- `test timeout override: 5 s`: 10

One complete ordered sequence was:

```text
EASYSTICK_WDT PRETIMEOUT
EASYSTICK_WDT CAPSULE_COMMIT seq=1 reason=1
rst:0x7 (HP_SYS_HP_WDT_RESET),boot:0xc (SPI_FAST_FLASH_BOOT)
easystick-boot: CRASH_CAPSULE VALID seq=1 reason=1 ...
easystick-boot: CRASH_CAPSULE FLASH_OK ...
```

Each live marker was emitted twice per logical event in the captured stream,
consistent with the duplicated console output also seen for the timeout
override. The required ordering and reset-to-next-boot relationship remained
unambiguous.

## Restoration and verdict

After capture, the complete 16 MiB stock image was written to the P4. esptool
reported `Hash of data verified`; an independent full-range `verify_flash`
reported `verify OK (digest matched)`. A final hard reset left the P4 on the
stock image. The C6 was never written.

P0 **PASS**:

`PRETIMEOUT` observed → `CAPSULE_COMMIT` observed → natural
`HP_SYS_HP_WDT_RESET` → next boot `CRASH_CAPSULE VALID` with `reason=1` →
`FLASH_OK`.

P1 and all later SSH/NTP/SMP experiments remain prohibited until separately
started after this P0 result.
