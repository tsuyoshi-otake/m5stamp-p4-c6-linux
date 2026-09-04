# ESP32-P4 LP-TSENS oracle

Status: **Phase-0 and first Linux Phase-1 hardware PASS recorded; three-run regression pending**

The optional boot-shim oracle is enabled only when
`EASYSTICK_TSENS_ORACLE=1` is present during the C68 boot-shim build. It is
not part of the normal SMP baseline. The oracle:

1. Enables the P4 SAR/TSENS internal REGI2C path.
2. Enables and resets LP-TSENS.
3. Writes a temporary DAC value (`7`) and requires readback `7`, so a
   power-on default cannot masquerade as proof of a working write path.
4. Writes the fixed Phase-0 range `offset=0`, `reg_val=15`, and requires
   readback `15`.
5. Powers the sensor, enables sampling, waits 300 us, and collects eight
   raw samples.
6. Records the eFuse field and its ESP-IDF-compatible decode:
   bit 8 is the sign flag, bits 7:0 are magnitude, and bit 9 is ignored.
7. Converts the averaged raw sample to integer milli-degrees Celsius and
   disables the sensor, clock, and internal REGI2C path before Linux starts.

`LP_TSENS.ctrl.ready` is recorded but is not a hard gate. ESP-IDF v5.5.3's
`temperature_sensor_get_celsius()` reads `LP_TSENS.ctrl.out` through its raw
read path and does not test `ready`; on this board the raw output is live while
`ready=0`. Requiring `ready=1` would therefore reject the reference sequence
that ESP-IDF itself uses.

The integer conversion is the ESP-IDF P4 formula for the fixed range:

```text
temp_mc = round(raw * 4386 / 10) - 20520 - delta_t * 100
```

The `/10` is correct because the numerator is already in milli-degrees
(`0.4386 C/sample = 438.6 mC/sample`). The oracle intentionally uses no
floating point.

## Static gate

Run from the repository root:

```text
py -3 projects/easystick-stamp-p4/firmware/linux/tests/test-tsens-oracle.py
```

Expected result:

```text
TSENS_ORACLE_STATIC_PASS
```

This test covers source markers and independent eFuse/conversion vectors. It
does not prove the ESP32-P4 analog hardware or the compiler output.

## Diagnostic build and capture

Use a fresh C68 output directory and the same isolated source/build procedure
as the current `m1-topcapfix` baseline, with this additional environment
variable:

```text
EASYSTICK_TSENS_ORACLE=1
```

`build-c68.sh` must print that the oracle was linked. The resulting UART
capture must contain exactly one successful reference line of this shape:

```text
P4_TSENS_REF PASS efuse_raw=... efuse_sign=... delta_t=... range_offset=0 range_reg=15 dac_before=... dac_probe=7 dac_readback=15 ready=... raw_min=... raw_max=... raw_avg=... temp_mc=...
P4_TSENS_REF CLEANUP sensor=off clock=off regi2c=off
```

The capture is a Phase-0 PASS only when:

- the temporary DAC probe reads back `7`;
- the selected range reads back `15`;
- `ready` is recorded; it is diagnostic only because the ESP-IDF reference
  driver does not gate raw reads on it;
- the raw range is non-degenerate and the reported temperature is within
  `-40000..125000` m°C;
- cleanup is observed;
- Linux still reaches the existing SMP smoke/stress endpoint without panic,
  WDT, RCU stall, or `smp.c:176`.

Record the exact four flash-input SHA-256 values, capture filename, UART
`.bin`/`.txt`/`.json` outputs, and any control capture used for comparison.
Do not call the oracle a Linux temperature-driver result. The independent
ESP-IDF reference is the prerequisite for the separate Linux Phase-1 probe.

## Phase-0 hardware evidence

The first oracle build reached the sensor but incorrectly treated `ready=0` as
fatal. The control capture is retained as:

```text
c68-0045-0047-l3-stress-20260819-m1-tsens-diag2-120s.txt
P4_TSENS_REF FAIL stage=sensor-not-ready ready=0 raw_min=117 raw_max=117 raw_sum=936 ctrl=0x00419675 wakeup=0x403fc000 sample_rate=20
```

After aligning the gate with the ESP-IDF raw-read contract, the same Linux
inputs plus the corrected oracle produced:

```text
c68-0045-0047-l3-stress-20260819-m1-tsens-diag3-120s.txt
P4_TSENS_REF PASS efuse_raw=0x021 efuse_sign=0 delta_t=33 range_offset=0 range_reg=15 dac_before=15 dac_probe=7 dac_readback=15 ready=0 raw_min=120 raw_max=121 raw_avg=120 temp_mc=28812
P4_TSENS_REF CLEANUP sensor=off clock=off regi2c=off
M5STAMP_STRESS PASS seconds=90 iterations=263 top_snapshots=4 worker_execs=263 smoke_calls=263
```

Capture manifest: `c68-0045-0047-l3-stress-20260819-m1-tsens-diag3-120s.json`
(`classification=helper-pass`, `command_sent=true`, `cleanup_seen=true`).
The diagnostic boot-shim input SHA-256 is
`3ffafb423ba3c94380373b115118defc3c8d9eb7ac749aaf70f1c838e3493a2a`.
The unchanged Linux inputs are `Image`
`781eeefcec0d2d800a509d43cdb7dbc7418ba73138b6e2d627c01c16fb141df5`,
`rootfs.squashfs`
`3fef4d270f8cdb549917da2d6d0e8e5013975b6e559a5bd748a4905eab6a7cc2`, and
DTB `675a04475b8671bcbbc01830c5927c1e5fd9c47f3382cb289a10a5e8b8519ee0`.

## Phase-1 hardware evidence

The first Linux-driver capture used the isolated v6 C68 input set:

```text
c68-0045-0047-l3-stress-20260819-m1-tsens-linux-v6-120s.txt
P4_TSENS_REF PASS efuse_raw=0x021 efuse_sign=0 delta_t=33 range_offset=0 range_reg=15 dac_before=15 dac_probe=7 dac_readback=15 ready=0 raw_min=121 raw_max=122 raw_avg=121 temp_mc=29251
P4_TSENS_REF CLEANUP sensor=off clock=off regi2c=off
P4_TSENS_LINUX PASS efuse_raw=0x021 delta_t=33 range_reg=15 dac_before=15 dac_probe=7 dac_readback=15 ready=0 raw=124 temp_mc=30566
M5STAMP_STRESS PASS seconds=90 iterations=262 top_snapshots=5 worker_execs=262 smoke_calls=262
```

The capture manifest classified this as `helper-pass`; Linux registered the
`esp32p4-tsens` thermal zone and the boot reached two CPUs. This proves the
probe sequence and independent oracle comparison once, but v6 predates the
stress helper's aggregate temperature fields. The v7 input set adds those
fields and must pass three consecutive captures before this phase is closed.
