# M0 ROM recovery evidence — 2026-08-10

Status: **pass for the module USB-C/COM10 path; no flash writes performed**.

Ten consecutive `esptool 4.8.1 chip_id` operations connected to the ESP32-P4
rev1.3, uploaded the temporary ROM stub, reported the same MAC
`e8:f6:0a:e2:5e:73`, and hard-reset through RTS.  This validates the
module-USB-C recovery path currently available on the assembled target.  It
does not yet validate the carrier GPIO35/CHIP_EN manual strap or the C6
recovery path.

Raw host output remains outside Git:

```text
C:\Users\developer\tmp\easystick-p4-rom-recovery-20260810.log
SHA256 18005F8D0D037E25EBB772CB2CFC03FBA948FFB7459DDDC04CB8B2E0A1E16AA9
```

The test used `--chip esp32p4 --port COM10 chip_id` only.  It did not invoke
`write_flash`, `erase_flash`, or any other mutating esptool command.
