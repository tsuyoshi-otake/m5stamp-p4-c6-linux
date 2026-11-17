# #25 camera groundwork — Tab5 SC2356 (verified on hardware)

## Sensor identity — measured, not inferred

- `i2cdetect` on `/dev/i2c-0` (internal bus, SCL=GPIO32, SDA=GPIO31)
  shows nothing at `0x36` until the camera rail/reset is released,
  then `0x36` appears — this is the SCCB channel.
- Release = expander `0x43` (gpiochip512) line **P6** (`CAM_RST`,
  sysfs gpio 518) driven high. Boot state is 0 (camera held off).
- Chip-ID read over SCCB (16-bit register addressing, write-phase
  `i2cset -y 0 0x36 0x31 0x07` then read-phase `i2cget -y 0 0x36`):

      0x3107 = 0xeb
      0x3108 = 0x52

  PID `0xeb52` == SC202CS PID (`sc202cs.h: #define SC202CS_PID 0xeb52`,
  esp-video-components).  M5Stack ships the SC2356, which is
  register-compatible with SC202CS (esp-bsp uses the SC202CS driver
  for the Tab5; espressif/esp-video-components#54).  The fitted unit
  answers the SC202CS ID registers, so driver-side it IS an SC202CS.

## Wiring (schematic + M5Tab5-UserDemo BSP)

- SCCB: internal I2C0 @ `0x36`, SCL=G32, SDA=G31 (shared with
  ES8388/ES7210/expanders/IMU/RTC/INA226).
- Reset/power-gate: PI4IOE5V6408 #1 (`0x43`) P6 `CAM_RST`; high =
  sensor out of reset and powered.  No separate AVDD switch found —
  P6 is the only control line.
- XCLK: **dedicated 24 MHz oscillator X2 (PXT24M)** wired to
  CAM_MCLK_1V8 through U35.  `CAM_MCLK_GPIO36` has a 10K pull-up but
  its series resistors to the camera (R27/R109) are **not fitted** —
  GPIO36 is a dead end; the oscillator runs whenever the sensor is
  powered.  Confirmed by community test: M5Tab5-UserDemo works with
  `bsp_cam_osc_init()` stubbed out.
- Data: MIPI-CSI-2 **1 data lane** (all esp-video sc202cs configs
  are `mipi_1lane_24Minput_*`; both D0/D1 pairs are routed in the
  pinmap but the production configs use lane 0 + clock pair).

## Minimum useful capture path (no ISP)

P4 peripherals involved: MIPI-CSI-2 host (CAM/CSI controller at
0x500E_xxxx-region dedicated pins), ISP DVP bypass not needed if we
stay raw, AHB-GDMA or the CSI's own DMA writes frames to RAM.

1. Driver skeleton (`media/platform/esp32p4/`, V4L2 or a char dev):
   claim the CSI MMIO window + interrupt from DT, power the sensor
   via the `0x43` P6 line (already a named gpio).
2. SCCB client: i2c driver for sc202cs — needs only the ID check
   (done above) and one of the published 1-lane init tables
   (`sc202cs_mipi_1lane_24Minput_1600x1200_raw8_30fps.h`).
3. CSI-2 → memory: program lane count=1, the sensor's clock rate,
   and the CSI DMA address.  Raw Bayer frames land in a contiguous
   buffer; expose via mmap/read or a V4L2 video node with the
   `RAW8/RAW10 Bayer` fourcc.
4. Userspace: `v4l2-ctl --stream-mmap` or a tiny reader dumps a
   frame; validate by pointing the camera at light and checking
   Bayer noise structure.

No ISP debayer, no auto-exposure loop, no media-controller graph —
just SCCB init + CSI DMA + readout.  That is a buildable driver, not
a port of the whole esp_video/ISP stack.

## Effort estimate

- CSI host driver with raw-Bayer capture to DRAM: this is the
  bulk of the work — new MMIO block, DMA descriptors again must
  live in internal SRAM (same constraint as #24/B11), plus interrupt
  handling and a V4L2/char-dev front end: **~3–5 days** including
  on-board debugging (the CSI block has no Linux driver in this
  tree and the TRM register-level docs are thin; expect register
  reverse-engineering against the IDF `esp_driver_cam` sources).
- sc202cs sensor init + ID/controls over SCCB: **~0.5 day**
  (register tables already public).
- DT wiring + rails/`CAM_RST` plumbing + userspace dump tool:
  **~0.5 day**.

Total ~4–6 days to a proven raw-Bayer frame read.  Anything beyond
(debayer, scaling, exposure control) is an ISP port and is a
separate, larger item.

## Do NOT

- Do not clock the camera from GPIO36 — it is unconnected on this
  board (fixed 24 MHz crystal drives CAM_MCLK).
- Do not assume 2 CSI lanes — production configs are 1 lane.
- Do not leave `CAM_RST` high across suspend — the rail policy in
  the shim keeps it off; a driver must manage P6 itself.
