# SSH IRQ host-claim fix build — 2026-08-26

Status: **RUNTIME TESTED — P4-only flash PASS; SSH stability FAIL**

## Scope

The candidate adds
`0028-easystick-sdio-irq-no-nested-claim.patch` to the host-side
ESP-Hosted-NG series.  `esp_handle_isr()` is called while the MMC SDIO host is
already claimed by the SDIO IRQ worker, so its four register accesses now use
`LOCK_ALREADY_ACQUIRED`.  Non-IRQ callers, including the TX credit path, keep
`ACQUIRE_LOCK`.

This changes the P4 Linux host driver only.  It does not change the C6 image,
register addresses, command format, retry policy, or DMA configuration.

Patch SHA-256:

```text
dad18e5a6b4c62fa19c687711b3a8fd44bbacef3e15f0809a23b5987a5dfcf71
```

The existing measured failure boundary remains the reference evidence:

```text
NETDEV_XMIT
ENQUEUE_OK
DEQUEUE
```

followed by no `CREDIT_OK`, `CREDIT_NO_BUFFER`, `TOKEN_READ_ERR`, or
`CMD53_ATTEMPT`, then:

```text
rst:0x7 (HP_SYS_HP_WDT_RESET),boot:0xc (SPI_FAST_FLASH_BOOT)
```

Source evidence: `uart-tx-ledger-ssh-2026-08-25.bin`, 30164 bytes,
SHA-256 `1c392541abb5a78fe7ed5fb64a3eb4046b221463eb9ea449c9abb989ed5e7472`.
That capture localizes the failure to the TX credit-read region but does not
prove that the IRQ callback's nested claim is the final cause.

## Build result

- Build interval: `2026-08-26T12:34:42Z` to `2026-08-26T12:52:23Z`
  (`17m41s`), exit code `0`.
- Profile: `m3-lab`; boot-shim profile: `m2`.
- `EASYSTICK_IDMAC_NONCOHERENT_RING=1` with
  `EASYSTICK_CMD53_BB_ALLOW_IDMAC_NC=1`.
- `EASYSTICK_SDIO_FORCE_PIO=0`.
- Descriptor invalidation, RX descriptor diagnostic, TX/TCP/SSH ledgers,
  ESP-Hosted diagnostics, and CMD53 error provenance were all `0`.
- The 0028 patch applied cleanly and `esp32_sdio.ko` compiled.
- Built-source static check: **PASS** — four ISR replacements present and the
  TX credit lock mode remains unchanged.
- WDT crash-capsule fail-closed gate: **PASS**.
- Retention PA: `0x50108080u`; version: `6`; size: `0x120` (`288` bytes).
- Regenerated `final_shot_manifest.py verify --require-shot-c`: **PASS**,
  `shot_c_allowed=true`.

## Artifacts

Export directory:

`D:\Users\Developer\easystick-tmp-20260820\easystick-p4-wdt-0054-ssh-capsule-20260825\irq-no-nested-claim-2026-08-26\`

`SHA256SUMS.txt` verification: **23/23 PASS**.  The exported manifest was
also verified against the exported copies.

| Artifact | SHA-256 |
|---|---|
| `boot-shim.bin` | `1e5ed162b234be1c60408e70d91777ccf4c3b0e4219f56b80fa2fd0e0ecec608` |
| `Image` | `1bfb4eab3cb9b492ad7ca9dda1c001f36ae8d9b21bd4ca0b5fe3c7e9d8b199ef` |
| `easystick-stamp-p4.dtb` | `7c3bd1e042d71aef80ebbeb37cfb402cabb400e3a41dc82b4a4b5d59a18eb163` |
| `rootfs.squashfs` | `00bc3d558372fd82b8f741d1d64e55a9c3f01d0c1263f4c9134e74e9d55a0ce1` |
| `vmlinux` | `35728ff5727ec868b3df0e445085c66bd3d1cc72e7f3c5d145215445f47c2ec7` |
| `rendered-patches/0054-easystick-wdt-crash-capsule.patch` | `3d8cd4dd6c8331810773d484976f27519620b765160a5ae1392dfe29f0663c5e` |

## Runtime result

The separately authorized P4-only write was performed on `COM10`; no C6 erase
or C6 write was performed.  `flash-candidate.ps1` identified ESP32-P4 rev1.3,
MAC `e8:f6:0a:e2:5e:73`, and verified every written region except the
bootloader readback that the script intentionally excludes from its post-write
`verify_flash` command:

```text
0x00002000  bootloader.bin       write hash verified
0x00008000  partition-table.bin  verify OK
0x00010000  boot-shim.bin        verify OK
0x00090000  Image                verify OK
0x00810000  rootfs.squashfs      verify OK
0x00f10000  easystick-stamp-p4.dtb verify OK
```

The UART-first capture was:

```text
uart-irq-fix-boot-2026-08-26.bin
bytes 15740
sha256 e2c541067ff26487b0771c88c18edb43ef71706a061c352ef8d8e556027424ea
reset_strategy esptool-HardReset-compatible-RTS-DTR
```

Boot, Wi-Fi association, static IPv4 `10.255.10.161`, and Dropbear startup
were observed.  The capture contained no `HP_SYS_HP_WDT_RESET` and no
`rst:0x7`; the only reset line was the requested
`rst:0x17 (CHIP_USB_UART_RESET)`.  The boot-time retention records were
`CMD53_BB empty/invalid` and `CRASH_CAPSULE empty/invalid`, so they are not a
crash dump from this SSH attempt.

The single SSH reproduction produced:

```text
TCP22_UP elapsed=4.031
SSH_AUTHENTICATED elapsed=5.094
CHANNEL_OPEN elapsed=5.235
SSH_EXCEPTION=ConnectionResetError:[WinError 10054]
SSH_CLIENT_CLOSED elapsed=24.610
```

`EXEC_SENT` was not reached: the reset occurred while issuing
`channel.exec_command('id')`.  After the attempt, both ping and TCP/22 were
unreachable (`DestinationHostUnreachable`).  Therefore the fix removed the
previously observed P4 WDT evidence, but did not establish stable SSH or
successful command execution.  The remaining failure is now a network/SDIO
loss without a P4 reset in this capture; it must not be reported as an SSH
PASS.

The preserved stock 16 MiB readback remains the recovery control:

```text
sha256 229459f251eaf6222f0c07968702d72a3818e520da1788ce465027d969020c24
```
