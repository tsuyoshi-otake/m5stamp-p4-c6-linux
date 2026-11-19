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

A 1280x720 RAW8 frame is 921,600 B (1600x1200 = 1.92 MB does not
fit at all).  `dma_alloc_coherent()` on this NOMMU tree draws from
the **kernel** page allocator — the buddy allocator over the RAM
region the kernel manages — *not* from the userspace
`nommu_pool` regions.  Two consequences:

- The frame buffer is allocated at **probe** and freed at
  `remove()` — i.e. held for the life of the boot.  That is the
  only time an order-8 block exists: measured on board (B37),
  once userspace is up `/proc/buddyinfo` shows order-8 free
  count 0, largest free block 512 KiB, and NOMMU has no
  compaction, so a `CAM_PREPARE`-time allocation always failed
  `-ENOMEM`.  Cost: 921,600 B request rounds up to one order-8
  block = 1 MiB of kernel-pool PSRAM permanently; the user
  `nommu_pool` numbers are unchanged (the pools are disjoint
  reserved-memory regions).  `__GFP_NOWARN` is set and an
  allocation failure returns `-ENOMEM` cleanly.
- An earlier sketch proposed a fixed `reserved-memory` carve-out
  at the top of PSRAM — rejected: the boot shim places the
  display fb at a *runtime* PSRAM address and patches
  `display_fb_res` per boot, so a static reservation could
  silently overlap the live scanout buffer.  Buddy-allocated
  memory can never collide with a `no-map` region (the
  shim-patched `display_fb_res`, both NOMMU pools,
  `i2s0_dma_sram`, the boot-partition staging area) by
  construction.  For the same reason the camera has no opt-in
  switch: the buffer costs kernel headroom only, and a config
  gate would have to drop and re-probe the driver while an
  order-8 block still exists — only at boot, which is when it
  already probes.

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

## Step-2 design — `/dev/cam0` one-shot capture (implemented in 0140)

Register map verified against vendor `hw_ver3` structs; every value
below is what the oracle's IDF sequence measured.

### Memory map (cam node `reg`)

| idx | base | span | use | mapping |
|-----|------|------|-----|---------|
| 0 | `0x5009F000` | 0x400 | MIPI-CSI host | exclusive (`devm_platform_ioremap_resource`) |
| 1 | `0x5009F800` | 0x60 | CSI bridge | exclusive (same) |
| 2 | `0x500A1000` | 0x80 | ISP input module (bypass) | exclusive (same) |
| 3 | `0x50081000` | 0x500 | DW-GDMA (shared with dsi-scanout) | `devm_ioremap` — dsi-scanout claims it |
| 4 | `0x500E6000` | 0xD0 | HP_SYS_CLKRST | `devm_ioremap` — i2s0 claims it |
| 5 | `0x50115000` | 0x200 | PMU (ext_ldo) | `devm_ioremap` — tsens claims it when built |

Shared-block RMW safety is by construction: every word written
here is one no other driver touches *at runtime* — clkrst
`0x14/0x18/0x3C/0xA8/0xAC/0xC0` (i2s uses `0x1C`, `0xC8`,
`peri_clk_ctrl11-14` at runtime; `soc_clk_ctrl1` `0x18` is the one
word i2s0 also writes, but only once inside `esp32p4_i2s_probe` —
dsi-scanout writes no clkrst word at all, so nothing can
interleave), pmu `ext_ldo[1]` `0x1C0/0x1C4` (tsens uses `rf_pwc`
`0x15C`), gdma `cfg0` dmac_en + `chen0` per-channel atomic
write-enable encodings (dsi-scanout owns only its shim-given
channel; this driver refuses a channel that is enabled or equals
`espressif,scanout-chan`, and never writes the block reset bit).

Frame buffer: `dma_alloc_coherent()` at **probe**, freed at
`remove()` — on-board measurement (B37) showed the NOMMU buddy
cannot produce an order-8 block once userspace is up
(order-8 free count 0, largest free block 512 KiB, no compaction),
so the earlier open-time allocation always failed `-ENOMEM`.
`arch_sync_dma_for_device/cpu` bracket the transfer (non-coherent
DMA, same pattern as 0124 i2s).  See the Memory section for why
there is no static `reserved-memory` carve-out.

### Block order (mirror of the measured oracle sequence)

`CAM_PREPARE` ioctl:
1. `CAM_RST` (`reset-gpios` = `exp0` P6) high, 20 ms.
2. LDO chan 3 (MIPI D-PHY rail, 2.5 V): `PMU.ext_ldo[1]` @
   `0x501151C0`/`0x1C4`.  The DSI panel normally keeps it lit
   (xpd=1) and the driver leaves it alone.  If it is off the
   driver **fails closed**: `CAM_PREPARE` returns `-ENODEV`
   ("no DSI panel; refusing uncalibrated power-up") instead of
   programming the ideal-constant `dref=9, mul=6` — no eFuse
   readback exists here, and a wrong PHY voltage is worse than
   no camera.
3. clkrst `0x500E6000`: `soc_clk_ctrl0` @0x14 bit13 (gdma cpu),
   `soc_clk_ctrl1` @0x18 bit5 (gdma sys) + bit10 (csi_brg) +
   bit11 (csi_host), `peri_clk_ctrl03` @0x3C bit4 (csi dphy cfg
   clk; src_sel[3:2]=0 → PLL_F20M), `peri_clk_ctrl25` @0xA8
   src_sel=1 (PLL160M) + bit25 (isp clk en), `peri_clk_ctrl26`
   @0xAC = 2 → 80 MHz ISP.  Reset pulse `hp_rst_en0` @0xC0
   bit27/28/29 (host/brg/isp) — **never bit21 (gdma)**: the
   dsi-scanout channel shares that block.
4. CSI host `0x5009F000`: `phy_shutdownz`/`dphy_rstz`/`csi2_resetn`
   all 0; PHY test-clock reset (`test_ctrl0` 1→0); test write
   `0x44` ← `0x0e` (hs_freq_sel=7 for 576 Mbps) via
   `test_ctrl1`/`test_ctrl0` bit-bang; release shutdownz→rstz→
   resetn; `n_lanes=0` (1 lane); `vc_extension=1` (VCX off);
   `scrambling=0`; clear all `int_st_*` latches (write ~0).
5. ISP input `0x500A1000`: `clk_en=1`, `cntl=1`
   (`mipi_data_en=1`,`isp_in_src=0`), `hsync_cnt=7`,
   `frame_cfg=0x0013F2CF` (hadr=ceil(1280·8/32)−1=319,
   vadr=720−1=719).
6. CSI bridge `0x5009F800`: `host_ctrl=3`, `clk_en=1`,
   `dma_req_cfg=0x20200` (burst 512 + bridge flow ctl),
   `buf_flow_ctl=960`, `data_type_cfg=0x2F12` (0x12..0x2f),
   `frame_cfg=0x012D0500` (has_hsync + hadr=720 + vadr=1280 —
   IDF swaps height/width names), `endian_mode=0`.
7. DW-GDMA channel (`espressif,dma-chan`, default 1; probe refuses
   to arm a channel that is enabled or equals `scanout-chan`):
   `sar0=0x50104000`, `dar0=fb pa`, `block_ts0=115199`
   (frame/64−1), `ctl0=0x221B44` (sms=CSI-port0, dms=mem-port1,
   sinc fixed, dinc incr, 64-bit both, msize 512 items both),
   `ctl1=0x4108840` (arlen/awlen 16, ioc_blktfr), `cfg0=0`
   (contiguous), `cfg1=0x2A820084` (tt_fc=P2M_SRC, src_per=CSI,
   ch_prior=1, osr_lmt 5/5), `llp0=0`, `int_clr0=~0`,
   `int_st_ena0=0x80003FE3` (status only), `int_sig_ena0=0` — **no
   IRQ line used**.  The channel is **programmed but left
   disabled** at PREPARE and `csi_en` stays 0: armed-idle the
   channel sees a spurious request edge and auto-disables within
   ~1 s (`int_st0` bit30 — a normal end state, not an error, and
   no longer classed as one), after which a live stream would
   overflow the undrained bridge FIFO and wedge the interconnect
   (observed on board as an instant hard freeze).  `cam_capture()`
   enables channel + `csi_en` only for the transfer window.
8. `read()` → `cam_capture()`: `GDMA_CHEN_EN` + `csi_en=1`, then
   polls `ch.int_st0` (done bit1 / error bits 5..13,31),
≤3 s timeout.  The bridge is the transfer's source flow
controller and **discards the first post-stream-on frame**
(~6 lines in — `discard_int_raw` bit2 latches, the DMA block
ends short): the IDF hides it because `receive()` is called
~50 ms after stream-on and the partial frame lands in
`backup_buffer`.  So `cam_capture()` does not trust one done:
after each done it checks the channel's `DAR0`; if fewer than
`CAM_FRAME_BYTES` landed it re-arms (`cam_gdma_arm` already
disables + flushes first) and waits again until a full frame
has advanced.  On a full frame: `arch_sync_dma_for_cpu`,
`copy_to_user`, disarm (`csi_en=0`, `chen=0x100<<ch`).
`*f_pos==0` → capture a fresh frame; later reads drain the
captured buffer.

### Userspace contract (cam-dump)

`open /dev/cam0` → `ioctl(CAM_PREPARE)` (powers the rail, arms
the pipe — the frame buffer itself was claimed at **probe**,
see the Memory section) → SCCB via `/dev/i2c-0`
(chip-ID `0xeb52`, 131-reg init table, `0x0100=0x01` stream-on)
→ `read()` returns the armed frame → `close()` disarms and
drops `CAM_RST` — the camera is powered only while
`/dev/cam0` is open (the buffer lives until `remove()`).  A
second frame therefore needs a fresh
open → PREPARE → SCCB-table + stream-on cycle (cam-dump does a
full cold capture per `-n` frame; the sensor loses its registers
on the power drop).

### Failure policy

- Timeout → `-ETIMEDOUT` + kernel log dump: host `int_st_*`,
  phy_rx/stopstate, bridge `int_raw`/`int_st`/`buf_flow_ctl`,
  gdma `status0`/`int_st0`.
- Error bits → `-EIO` + same dump.
- Prepare never touches the GDMA reset line or a running channel;
  the LDO3 rail is never programmed (off ⇒ `-ENODEV`, fail
  closed); `CAM_RST` drops at `release()`/`remove()` (never left
  high across driver teardown).
