# P1 Zephyr control-image reconnaissance — 2026-08-23

Status: **P1-NG NEGATIVE CONTROL OBSERVED — C6 unchanged; full P1 not accepted**.

This record is the source and compatibility gate for the P1 independent
control image. It is not evidence of SDIO enumeration, ESP-Hosted
initialisation, Wi-Fi scan, association, or ping.

The current authorized condition is different from the original valid-control
plan: the M5Stack Stamp-P4 and its attached Stamp-AddOn C6 are retained, but
the C6 image is not backed up, erased, or rewritten. Only the P4 Zephyr host
may be written, to observe the boundary against the existing NG companion.

## Objective

Use the same EasyStick Stamp-P4 and Stamp-AddOn C6 hardware with an
independent Zephyr host implementation. Keep SSH, NTP, SMP, and Bluetooth
outside this gate.

The working hypothesis is:

> If a Zephyr ESP-Hosted-MCU host reaches the C6 over the documented
> GPIO42–48 SDIO wiring and completes scan, association, and ping, the
> remaining SSH failure is narrowed to the native-Linux path. If the Zephyr
> control image fails before those steps, the failure is not yet attributable
> to Linux.

## Source reconnaissance

| Item | Result |
| --- | --- |
| Zephyr feature reference | PR [#114532](https://github.com/zephyrproject-rtos/zephyr/pull/114532), merged |
| Zephyr revision | `d544481d9ad9c711cefe984c5ea926d71cb56341` |
| Zephyr host transport | `drivers/misc/esp_hosted_mcu/esp_hosted_mcu_sdio.c` |
| Zephyr binding | `espressif,esp-hosted-mcu` as a child of an SDHC slot |
| Matching C6 source | `espressif/esp-hosted-mcu`, external checkout revision `3f0d1076749afdb589f00c075d8dce895e3dd32d` |
| Existing EasyStick C6 evidence | ESP-Hosted-NG `network_adapter.bin`, SHA-256 `2ac39933c84c7688fb5d251ffc01bd15e53b665a272b6053dedca448e40e1827` |

The existing C6 artifact is ESP-Hosted-NG 1.0.6, while PR #114532 implements
ESP-Hosted-MCU's protobuf/RPC protocol. The existing NG image is therefore a
protocol negative control, not a valid P1 companion image. A full P1 run
requires an ESP-Hosted-MCU C6 image. The NG binary must be preserved outside
the repository before that image is written; a hash without the binary is not
a recovery backup.

For the current negative-control run, the existing NG image is intentionally
left in place and no C6 write gate is attempted. The matching
ESP-Hosted-MCU image remains a build/reference artifact only.

The first attempt to reconstruct the NG image from the checked-in sources was
stopped before any write because the nested ESP-IDF
`components/esp_wifi/lib` checkout is not initialised. No C6 replacement image
has been produced or written by this step.

## EasyStick overlay design

The Function-EV board DTS is used only as a board-support base. It is not
copied into this project. The EasyStick overlay will:

- disable the Function-EV SDHC slot-0/SD-card node;
- keep SDHC slot 1 in 4-bit mode;
- set `CLK=43`, `CMD=44`, `D0=45`, `D1=46`, `D2=47`, and `D3=48`;
- bind the hosted-C6 reset/control line to physical GPIO42
  (`&gpio1 10`);
- use `GPIO_ACTIVE_HIGH` for GPIO42 because the carrier line is the
  regulator-enable/power-gate path: low removes the C6 3.3 V rail and high
  releases it for the C6's RC reset network;
- leave `pwr-gpios` unset because GPIO42 is the dedicated host-controlled
  reset/power path.

The overlay must retain the Function-EV v1.3/360 MHz target and USB
Serial/JTAG console. No Function-EV SDIO pin or C6 reset value may remain
silently active after the EasyStick overlay is applied.

## A/B and acceptance

### A — valid control run

1. Build the Zephyr host at the pinned revision with the EasyStick overlay.
2. Build a matching ESP-Hosted-MCU SDIO slave image for the C6.
3. Preserve and verify the old NG image, then flash the matching C6 image
   through an independently verified ESP-Prog/UART-boot path.
4. Flash the Zephyr host image to the P4 only after its image and DT
   readback checks pass.
5. Capture raw UART output and sidecar SHA-256 evidence for:
   `C6 reset → SDIO enumerate → Function 1 enable → CMD52 → small CMD53 →
   512-byte CMD53 → repeated CMD53 → ESP-Hosted init → scan →
   association → ping`.

### B — negative controls

- The existing NG C6 image must not be called a P1 success with the Zephyr
  MCU driver; a protocol mismatch is expected.
- A build with the Function-EV slot-1 GPIOs
  `CLK=18`, `CMD=19`, `D0=14..D3=17` is a pin-map negative control and must
  not be flashed to the EasyStick.
- A deliberate overlay corruption must be rejected by the DT/source
  verification before any flash command is allowed.

### Current run — C6-unchanged negative control

1. Keep the attached C6 image unchanged; do not invoke the C6 backup, erase, or
   write path.
2. Verify the Zephyr host image, merged DTS, configuration, and source
   revisions.
3. Flash only the P4 host image with the explicit P4-only write gate and the
   preserved P4 stock readback available for recovery.
4. Capture raw P4 UART output through the first `EASYSTICK_P1 STEP .. FAIL`
   boundary.
5. Run `verify-p1-ng-capture.py`. A passing result means that a contiguous
   transport prefix and a subsequent failure were observed; it is not full
   P1 acceptance and does not prove that the failure is caused only by the
   protocol mismatch.

The current run result is recorded in
[`p1-ng-result-2026-08-24.md`](p1-ng-result-2026-08-24.md). The P4-only write
and verify passed, the raw UART capture contains the C6 reset through CMD53
markers, and the first protocol boundary is
`STEP 08 ESP_HOSTED_INIT FAIL got=0x000000 expected=0x020c0c`. The C6 write
count is zero. This result is classified as `P1-NG negative-control observed`,
not as full P1 acceptance.

### P1 PASS

PASS requires one captured run containing all ordered transport and network
markers, with no unclassified reset, CMD52/CMD53 error, or missing response,
and a successful ICMP ping after association. The capture filename,
byte-count, SHA-256, firmware hashes, and exact source revisions must be
recorded in a separate result report.

### P1 FAIL / not yet accepted

P1 is not accepted if the C6 image is still NG, if the C6 reset/SDIO source
cannot be verified, if any transport ladder step is absent, or if scan,
association, or ping is not observed. SSH, NTP, SMP, and Bluetooth results
cannot repair a P1 failure because they are outside this gate.

The current negative-control result is recorded separately as
`P1-NG negative-control observed` or `P1-NG capture inconclusive`; neither
state changes the full P1 acceptance rule above.

## Next authorized work

The current C6-unchanged run is complete. Its result is:

1. P4 stock recovery readback and the Zephyr host image were verified;
2. only the P4 host image was written and verified;
3. the raw UART capture was verified through the protocol-mismatch boundary.

The original C6 replacement sequence remains reserved for a future full P1
run and must not be performed under this condition.
