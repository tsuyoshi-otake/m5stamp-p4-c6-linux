# ESP32-P4 Linux TSENS bring-up — 2026-08-19

Status: **WIP — Linux driver and three-run C68 SMP regression PASS recorded**

This document records Phase 1 after the independent ESP-IDF boot-shim oracle.
The oracle remains a reference measurement only; Linux must configure the
analog range itself and must not inherit a DAC setting from the shim.

## Selected implementation

The opt-in kernel patch is:

`kernel-patches/0050-thermal-esp32p4-add-lp-tsens-driver.patch`

Enable it with:

```text
EASYSTICK_TSENS_LINUX=1
```

The C68 build path then appends:

```text
CONFIG_THERMAL=y
CONFIG_THERMAL_OF=y
CONFIG_ESP32P4_TSENS=y
```

The board DTS supplies named windows for:

- LP-TSENS: `0x5012f000`
- LP_PERI clock/reset: `0x50120000`
- LP analog I2C master: `0x50124000` (including `CLK160M` at offset `0x34`)
- PMU: `0x50115000`
- eFuse: `0x5012d000`

This is deliberately a vendor bring-up arrangement. The kernel tree does not
yet have a reusable P4 clock/reset provider, eFuse nvmem provider, or shared
REGI2C service.

## Probe contract

The driver must complete this sequence before registering
`esp32p4-tsens` under `/sys/class/thermal`:

1. Enable the PMU peripheral I2C path.
2. Enable the LP-I2C-master and LP-TSENS clocks.
3. Pulse both the LP-I2C-master and LP-TSENS resets.
4. Select the 160 MHz LP-I2C master clock and select the SAR/ADC analog block
   `0x69` in `ANA_CONF2` bit 7.
5. Read the current TSENS DAC field.
6. Write probe value `7` through the REGI2C read-modify-write operation and
   require readback `7`.
7. Write the selected fixed range value `15` and require readback `15`.
8. Read the 10-bit eFuse `temperature_sensor` field and decode it as bit 8
   sign plus low-8-bit magnitude; bit 9 is retained in the raw log but ignored
   by the ESP-IDF conversion. It is not a two's-complement field.
9. Power up TSENS, enable wakeup and sampling, and wait at least 300 us.
10. Read `OUT`, convert to integer milli-degrees Celsius, and reject an
    impossible initial value.
11. Register a tripless thermal zone.

The hot `get_temp()` path only reads `TSENS.CTRL.OUT` and performs integer
conversion. It does not access REGI2C, reset the block, or reconfigure power.
The locked Linux tree currently has no other `REGI2C`, `regi2c`, or
`I2C_ANA_MST` consumer under `drivers/`; this is an explicit bring-up
assumption, not a claim that the private transaction is an upstream shared
service.

## Static gate

Run from the repository root:

```text
py -3 projects/easystick-stamp-p4/firmware/linux/tests/test-tsens-linux.py
```

Expected result:

```text
PASS: TSENS Linux driver contract and 6 negative mutants
```

The negative mutants cover the eFuse sign bit and field width, the DAC final
readback, the REGI2C offset, the 300-us settle delay, and the driver hunk line
count. The test is a source-contract gate; it is not a compiler or hardware
result.

Run the independent expected-value vectors as well:

```text
py -3 projects/easystick-stamp-p4/firmware/linux/tests/test-tsens-linux-conversion.py
```

It checks the ESP-IDF-specific eFuse sign/magnitude cases, including ignored
bit 9, and fixed offset=0 integer conversion vectors. These are offline
reference vectors, not evidence that the kernel binary has executed.

## Hardware capture

Build a separate output directory from the same C68 SMP baseline:

```text
export EASYSTICK_TSENS_LINUX=1
projects/easystick-stamp-p4/firmware/linux/build-c68.sh \
  --vendor <output-dir> --profile m1
```

Flash only the generated boot-shim, `Image`, rootfs, and DTB input set. Do not
replace the three-image `m1-topcapfix` baseline directory. The Linux probe
must emit one line similar to:

```text
P4_TSENS_LINUX PASS efuse_raw=0x021 delta_t=33 range_reg=15 dac_before=15 dac_probe=7 dac_readback=15 ready=0 raw=101 temp_mc=...
```

The `ready` bit is recorded but is not a hard gate, matching the Phase-0
ESP-IDF observation that raw output was live while `ready=0`.

After the probe line, verify from the shell:

```sh
cat /sys/class/thermal/thermal_zone*/type
cat /sys/class/thermal/thermal_zone*/temp
for i in $(seq 1 10); do
	cat /sys/class/thermal/thermal_zone*/temp
	sleep 1
done
```

The value is required to be milli-degrees Celsius. A result such as `45` or
`45000000` is a unit failure.

The first hardware probe capture (v6) recorded the following independent
comparison:

```text
P4_TSENS_REF PASS efuse_raw=0x021 delta_t=33 range_offset=0 range_reg=15 dac_before=15 dac_probe=7 dac_readback=15 raw_min=121 raw_max=122 raw_avg=121 temp_mc=29251
P4_TSENS_LINUX PASS efuse_raw=0x021 delta_t=33 range_reg=15 dac_before=15 dac_probe=7 dac_readback=15 raw=124 temp_mc=30566
```

The oracle emitted `P4_TSENS_REF CLEANUP`; Linux then reached
`Brought up 1 node, 2 CPUs`, `/sbin/init`, and a 90-second
`M5STAMP_STRESS PASS seconds=90 iterations=262 top_snapshots=5` with no
panic, WDT, RCU-stall, illegal-trap, or `smp.c:176` marker. The source capture is
`c68-0045-0047-l3-stress-20260819-m1-tsens-linux-v6-120s.json`; this is one
probe result, not yet the final repeatability gate.

## Phase-1 acceptance

Phase 1 is not PASS until a capture proves all of the following:

- Linux prints `P4_TSENS_LINUX PASS`.
- DAC probe readback is `7`; final readback is `15`.
- eFuse raw and `delta_t` agree with the independent boot-shim oracle.
- A thermal zone exists and returns integer milli-degrees Celsius.
- Ten reads complete without errors or impossible jumps.
- The stress summary reports `temp_reads > 0`, `temp_failures = 0`, and
  sensible start/min/max/end milli-degree values.
- Linux does not depend on the boot-shim's previous analog configuration.
- The C68 SMP boot remains free of panic, WDT, RCU stall, illegal-trap, and
  `smp.c:176` markers.

The three post-driver `m1-topcapfix` stress captures are a separate gate and
must use the exact same flash inputs and 90-second helper condition as the
pre-TSENS baseline.

## Final repeated regression

The first v7 artifact set (`m1-tsens-linux-v7`, Image
`a54cea9b...`, rootfs `40bddda0...`) produced one diagnostic failure during
the first 90-second stress run: a userspace instruction-access fault in the
persistent CPU1 worker was followed by an RCU stall and
`HP_SYS_HP_WDT_RESET`. Its capture classification was
`watchdog-reset`; it is retained as negative evidence and is not silently
counted as a pass.

The helper was then rebuilt from the same committed source, with a fresh
coordinated quartet and exact SHA-256 verification before each flash:

```text
boot-shim.bin  20342d54d20775d3775873da5658bf6df2936236436166e12ca7a263dbffdb6c
Image          e55ae1674a39790030d70a9b1dac58f14502891ffe56e3215a12eb579dd2da8c
rootfs.squashfs
               6cb7fed9a0ec5c5ecab6d13f4fdffa774d1b2d5d172a7fa8a798b1c188e3bafd
easystick-stamp-p4.dtb
               cf57cc0e87b66252345a1573ccd725f162a9d12918eba4bc5164da931dfbfc24
```

Three consecutive 120-second captures using that same quartet and
`/usr/sbin/m5stamp-smp-stress --seconds 90` classified as `helper-pass`:

```text
repeat2: iterations=262 worker_execs=262 smoke_calls=262
         temp_reads=92 temp_failures=0 temp_start_mc=30566
         temp_min_mc=30566 temp_max_mc=31005 temp_end_mc=31005
repeat3: iterations=263 worker_execs=263 smoke_calls=263
         temp_reads=92 temp_failures=0 temp_start_mc=30128
         temp_min_mc=30128 temp_max_mc=31005 temp_end_mc=31005
repeat4: iterations=263 worker_execs=263 smoke_calls=263
         temp_reads=92 temp_failures=0 temp_start_mc=30128
         temp_min_mc=30128 temp_max_mc=31005 temp_end_mc=31005
```

All three also observed `Brought up 1 node, 2 CPUs`, CPU1 worker execution,
the existing timer/IPI markers, and no `Guru Meditation`, HP WDT reset, RCU
stall, illegal-trap, or `smp.c:176` marker. The short 10-second diagnostic
capture independently returned `temp_reads=12` and `temp_failures=0`.

This closes the Phase-1 hardware regression for the regenerated quartet, not
manufacturing approval. The initial v7 failure remains a reproducibility
residual to investigate if the same pre-rebuild artifact lineage is reused.
