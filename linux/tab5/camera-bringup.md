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

## Measured capture path (CAMERA_ORACLE, verified B20)

The oracle (`linux/boot-shim/main/camera_oracle.c`, off by default,
`CAMERA_ORACLE=1` env at shim build) now captures a real 921,600-byte
1280x720 RAW8 frame and proves it with a deterministic test pattern.
The measured pipeline — every hop required:

    SC202CS -> MIPI D-PHY -> CSI host (0x5009F000)
        -> **ISP input module, bypass mode** -> CSI bridge
        -> DW-GDMA ch1 -> PSRAM buffer

Blocks Linux must program, in order:

1. **Sensor power**: expander `0x43` P6 `CAM_RST` high, ~20 ms
   ramp.  MIPI D-PHY LDO = LDO channel 3 at 2.5 V (same rail the
   DSI D-PHY uses).
2. **SCCB**: internal I2C0 @ `0x36`, 16-bit reg addressing.  Chip ID
   `0xeb52` at 0x3107/0x3108.  Published 1-lane 24 MHz-input
   1280x720 RAW8 table (131 regs), then `0x0100=0x01` to stream.
3. **CSI host**: 1 data lane (`n_lanes` reg encodes lanes-1, so the
   correct write is 0), `lane_bit_rate_mbps=576`.  PHY
   `hs_freq_sel=7` (readback via testbus reg 0x44 = 0x0e).  PHY
   clock source = `PLL_F20M` (`clk_src=0`); the driver enables it
   via `esp_clk_tree_enable_src`.
4. **ISP input module — REQUIRED even for raw.**  The CSI host's
   pixels reach the bridge *through* `ISP.input`: without it the
   host decodes packets (bndry/seq errors visible) but `brg int_raw`
   stays 0 and DW-GDMA never moves.  What the IDF bypass branch
   programs (`esp_isp_new_processor`, `flags.bypass_isp`):
   `cntl.isp_in_src=0` (CSI source) + `cntl.mipi_data_en=1`, ISP
   module clock on, Hsize=`ceil(h_res*bpp/32)`, Vsize=`v_res`,
   Bayer order BGGR, `clk_hz=80 MHz`.  `esp_isp_enable()` is NOT
   called for a bypassed processor — it refuses.
   Measured after one frame: `ISP cntl=0x00000001`,
   `clk_en=0x00000001`, `frame_cfg=0x0013f2cf`,
   `hsync_cnt=0x00000007`, `int_raw=0x18000000`, `int_st=0`.
5. **CSI bridge**: claimed by the ISP path as
   `MIPI_CSI_BRG_USER_SHARE` — `host_ctrl=0x3` (module clocks +
   reset pulse), `clk_en=1`, `csi_en=1`, `frame_cfg` =
   h_pixel/v_row fields (note the IDF hal maps frame_height ->
   h_pixel, frame_width -> v_row — names are swapped in the
   struct).  `has_hsync_e` (fcfg bit 24) defaults to 1 and is fine
   left alone once the ISP feeds real data.
6. **DW-GDMA ch1**: bridge FIFO `0x50104000` -> dst buffer,
   64-bit width, burst_len 16, items_512, fixed src/increment dst.
   **Upstream `drivers/dma/dw-axi-dmac` does NOT match** — the P4
   block is a Synopsys DW-GDMA hw_ver3 register variant; the DSI
   scanout driver (0095) already bit-bangs its registers for the
   same reason.  Model on IDF `esp_driver_ddw_gdma` /
   `dw_gdma_link_list_item_t` (64-byte LLI, uncached alias
   +0x40000000).
7. **Interrupts**: GDMA transfer-done on the channel -> frame
   complete.  With `on_get_new_trans` registered the engine
   free-runs — the ISR takes the next buffer from the callback
   (its bool return is ignored); a Linux driver that wants
   one-shot must leave the queue dry or disable the channel.
   Clear `int_st_*` fatal latches in the CSI host before
   stream-on: a mid-frame join latches `seq_frame_fatal` and
   freezes the pipeline.

Order that worked: CSI+ISP armed -> clear host `int_st_*` ->
`0x0100=0x01` -> ~50 ms ramp -> frames.

Verified sensor controls (sc202cs driver + oracle):
- test pattern: `0x4501` bit 3 -> horizontal ramp
  (`vstripes: 0 0 0 0 0 14 34 54 74 ... 214`, hstripes uniform)
- gain: `0x3e06` dig coarse / `0x3e07` dig fine / `0x3e09` ana
  coarse; map in `sc202cs_gain_map` (1x={0x80,0x00,0x00} ->
  mean-16=2; 32x={0xfc,0x01,0x0f} -> mean-16=131, black level 16)
- exposure: `0x3e00-0x3e02`, 1/16-line units, max = VTS-6;
  VTS at `0x320e/0x320f` = 1250 (720p30)

## XCLK — measured

Dedicated 24 MHz oscillator (X2/PXT24M) through U35, confirmed on
hardware: an `ORACLE_SKIP_XCLK=1` build that never drives GPIO36
captures identical frames (chip ID, test pattern, gain response).
GPIO36 is unconnected (R27/R109 not fitted).  The M5 BSP's
`bsp_cam_osc_init` drives 24 MHz there anyway because the BSP is
shared with boards that need it — on Tab5 it is a no-op.
**Linux does not need a clock output for the camera.**

## Memory

Linux has ~1.5 MB free in the NOMMU pool; a 1280x720 RAW8 frame is
921,600 B (1600x1200 = 1.92 MB does NOT fit).  Frame buffers must
be a reserved-memory carve-out outside the Linux pool, exactly
like the panel framebuffer (`fb @0x49000ec0`, handed over in the
DT).  Same node pattern: `reserved-memory` child +
`memory-region` reference on the camera node, ~1 MB.

## Interface proposal

`/dev/cam0` char device, smaller than V4L2 on this NOMMU tree:
`read()` returns one complete raw frame (start -> arm DMA ->
sleep on GDMA done -> return buffer).  No mmap needed for v1
(read() copies out of the carve-out; the pool is tight but a
single 921,600 B frame staged through a 64 KB bounce buffer works).
V4L2 (`/dev/video0` + `RAW8 Bayer` fourcc) is the follow-on if a
real consumer needs streaming; nothing on the board today does.

## Effort estimate (updated)

- CSI host + PHY + ISP-input + bridge + DW-GDMA raw capture
  driver: the bulk — one MMIO cluster now fully documented by the
  oracle dump, DW-GDMA needs a fresh driver (no upstream match),
  interrupts + `int_st` hygiene per above: **~3–4 days**.
- sc202cs SCCB driver (ID + table + controls above): **~0.5 day**.
- DT (`reserved-memory` + camera node) + `/dev/cam0` + dump tool:
  **~0.5 day**.

Total ~4–5 days to a proven raw-Bayer frame read — one day lighter
than estimated pre-oracle because the pipeline is now measured,
not guessed.  Debayer/ISP processing stays a separate, larger item.

## Do NOT

- Do not clock the camera from GPIO36 — it is unconnected on this
  board (fixed 24 MHz crystal drives CAM_MCLK).
- Do not assume 2 CSI lanes — production configs are 1 lane.
- Do not leave `CAM_RST` high across suspend — the rail policy in
  the shim keeps it off; a driver must manage P6 itself.
