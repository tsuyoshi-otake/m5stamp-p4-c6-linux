# C6-side observation plan: identity, console route, read-only checks (2026-08-11)

Status: **Sections 1 and 2 stand as verified findings. Sections 3 and 4 are
DEFERRED and MUST NOT be executed.**

The P4 side of Issue #6 is measured and committed (`dff029a`). SDIO enumeration
works, the empty-IRQ storm is closed, `INT_RAW` and `INT_ST` are genuinely
zero, and `PACKET_LEN` is zero on eleven reads by two independent observers.
Every remaining hypothesis lives on the C6, and the C6 has never been observed
directly.

> **Superseded on 2026-08-12, before any of this was executed.** The paragraph
> above is wrong in its conclusion, and the reason is worth keeping: those zero
> reads were a **host-side** defect, not C6 silence. A 4-byte slave-register read
> had been refactored from four CMD52 transfers into one CMD53 transfer, and the
> CMD53 form reads back zero on this host — so `INT_ST` and `PACKET_LEN` were
> zero because the host could not read them. Reverting that one change
> (`0012-…-register-cmd52-access.patch`) makes `PACKET_LEN` read 40 and `Tx Pos`
> read 10 on the current stack. `wlan0` is still absent, so the C6 is not cleared
> of everything — but "eleven zero reads by two observers" was eleven readings
> taken through one broken instrument, which is why agreement between the two
> observers proved nothing (root `CLAUDE.md` §14.2, §14.13). See
> `a0-lastgood-restore-and-rootfs-bisect-2026-08-12.md` §4 and §9.8. Sections 3
> and 4 below remain DEFERRED and MUST NOT be executed.

This document does three things and takes no hardware action:

1. Establishes what firmware is *expected* on the C6, from build configuration
   and artifact metadata rather than from a release or version name.
2. Establishes how the C6 can be observed, from the AddOn's own schematic.
3. States the exact read-only checks, each mapped to the question it answers —
   **retained as a record of what was proposed, not as an instruction.**

No wiring was made, no firmware was written, and the C6 was not touched while
producing this document.

## 0. Deferral

The electrical and UART observation route in Sections 3 and 4 is **deferred on
physical-access grounds**, decided 2026-08-11 by the operator, who has the
assembled hardware in front of them and is the only party able to judge it.
The DMM and test-point approach is not practical on this assembly, and the
probe, wiring, and short-circuit risk it would add is not worth accepting for
telemetry. That judgement is a fact about the hardware, not a finding this
document could have reached: Section 2.1 already records that the extraction
method cannot see which side of the AddOn a test point sits on, its size, or
whether the mated stack covers it, and Section 3 already made physical
reachability a precondition rather than an assumption.

Therefore, and until the operator says otherwise:

- Do not make any wiring connection to TP1..TP9 or to J1.
- Do not probe the assembled board.
- Do not modify or reflash the C6.
- Do not start a logic-analyzer capture.

**Consequence, stated rather than papered over.** The three remaining C6-side
hypotheses in `m2-sdio-boot-2026-08-10.md` — a C6 image that is not an
ESP-Hosted-NG SDIO slave build, a C6 held in or re-entering reset, and a slave
that boots but never queues the bootup event — stay **unresolved**. Nothing in
Sections 1 or 2 discriminates between them. Section 1 raises confidence about
what was *built* and *sent*; it observes nothing about what is *running*. The
C6 has still never been observed directly, and no host-side measurement can
substitute, because a C6 that panics early and a C6 that received the wrong
transport build produce host telemetry indistinguishable from the nine captures
now recorded.

Sections 1 and 2 are unaffected by the deferral. Both were produced by reading
committed build configuration, locked submodule sources, and a vendor PDF; no
part of either depended on touching the board. Section 2.3 in particular is a
correction to how the P4 boot shim's own GPIO42 pulse should be understood, and
it applies to software work that continues.

### What remains available without probing or wiring

Recorded so that "deferred" is not read as "no path forward". None of these has
been started, and none should be started without a decision:

| Candidate | Needs | What it could discriminate |
| --- | --- | --- |
| Raise the DTS `max-frequency` above the 400 kHz identification clock | P4-only rebuild and flash over the existing USB-C cable | Whether the bus rate cap contributes; the only run that ever produced `wlan0` was not capped this way |
| Rebuild without `easystick,force-pio` | same | The DMA path, which the one historical `wlan0` run used and every recent capture has not |
| Extend the `BOOT_POLL` window well past 2.7 s | same | A boot frame arriving later than any capture has waited |
| Rebuild the C6 image and compare it byte-for-byte against the retained `network_adapter.bin` | container build only, no flash | Whether the retained artifact is reproducible from the locked submodules — a check on the artifact, not on the C6 |

The first three are hardware experiments in the sense that they end in a P4
flash and a capture, but they need no probe, no wire, and no C6 write. The
fourth touches no hardware at all.

## 1. Expected C6 firmware

### 1.1 What is verified

| Property | Value | How it was established |
| --- | --- | --- |
| Image | `network_adapter.bin`, 1,039,040 bytes | `tests/c6-sdio-build-2026-08-10.md` |
| Image SHA-256 | `2ac39933c84c7688fb5d251ffc01bd15e53b665a272b6053dedca448e40e1827` | same |
| Source tree | ESP-Hosted-**NG** `esp_hosted_ng/esp/esp_driver/network_adapter` | `m2/build-c6.sh` |
| NG commit | `8626b42fd3f9eb5a1ccb5daea481f0d8d32b1685` | locked submodule |
| ESP-IDF commit | `2c211b236707889e8400c4dc5644dd5c4ee071e0e` (v5.5.3) | same |
| Transport | SDIO, **not** SPI | build gate, below |
| App descriptor | `version='NG-1.0.6.0.1'`, `project_name='network_adapter'` | OTA host parsed it out of the image |

The transport claim does not rest on the branch name `release/ng-1.0.6`.
`m2/build-c6.sh` asserts it against the configuration the build itself
generated:

```bash
grep -Eq '^#define CONFIG_ESP_SDIO_HOST_INTERFACE 1' "${output}/build/config/sdkconfig.h" \
  || { echo "C6 build did not select SDIO transport" >&2; exit 1; }
if grep -Eq '^#define CONFIG_ESP_SPI_HOST_INTERFACE 1' "${output}/build/config/sdkconfig.h"; then
  echo "C6 build unexpectedly selected SPI transport" >&2; exit 1
fi
```

That is a positive assertion plus its negative control, read from
`build/config/sdkconfig.h` — a generated file, so it reflects the Kconfig
resolution that actually produced the binary. It agrees with the NG slave's own
`main/Kconfig.projbuild`, which sets `default ESP_SDIO_HOST_INTERFACE if
SOC_SDIO_SLAVE_SUPPORTED`; the C6 satisfies that condition.

The app descriptor is independent of both: it was read back out of the
`slave_fw` partition by the ESP-Hosted-MCU OTA host at run time and printed as
`version='NG-1.0.6.0.1'`, five segments, C6 load addresses
`0x420c0020 / 0x40800000 / 0x42000020 / 0x408011f8 / 0x408219e0`. So the
image's own metadata, the build's own generated config, and the upstream
Kconfig default all say the same thing by three different routes.

### 1.2 What is inferred, not verified

Delivery is sourced but not confirmed from the C6 side. Two OTA-host captures
exist (raw logs outside Git, hashes in `m2-sdio-boot-2026-08-10.md`):

| Attempt | Runtime SDIO pins | Result |
| --- | --- | --- |
| v1 | `CLK[18] CMD[19] D0[14] D1[15] D2[16] D3[17]` — wrong | `ensure_slave_bus_ready failed`, `Timeout waiting for Resp for [0x110](Req_OTABegin)`, `OTA failed with error: ERROR` |
| v2 | `CLK[43] CMD[44] D0[45] D1[46] D2[47] D3[48] Slave_Reset[42]` — correct | `Partition OTA completed successfully - Sent 1039056 bytes`, `Slave firmware version: 2.12.1`, `New firmware activated - slave will reboot` |

`2.12.1` is the **pre**-OTA factory ESP-Hosted-MCU/FG slave that performed the
update; it is not the NG image. The three P4 boots after v2 each show
`Card init success, TRANSPORT_RX_ACTIVE` followed by `transport: Not able to
connect with ESP-Hosted slave device`, which is the expected result if the C6
is now NG, because an MCU/FG host cannot speak the NG protocol. That
corroborates the delivery; it does not prove it. Everything after
`New firmware activated` is inference, and the C6 console is the first thing
that could turn it into an observation.

One risk this exposes: the OTA wrote an **application** into an OTA slot. It
did not write NG's partition table. NG's
`partitions.esp32c6.csv` declares `nvs@0x9000`, `otadata@0xd000`,
`phy_init@0xf000`, `factory@0x10000`, `ota_0@0x130000`, `ota_1@0x250000`; the
C6 retains whatever table the factory FG image shipped at `0x8000`. If those
tables disagree on `phy_init` or `nvs`, the NG app boots against partition
entries it was not built for. NG sets `CONFIG_ESP_WIFI_NVS_ENABLED=n`, which
narrows the exposure but does not remove it.

## 2. Observation route, from the AddOn schematic

Source: `c6-addon-schematic.pdf`, 561,763 bytes, SHA-256
`bcb1e4156259fe2f648c7eabda1870552fdbcb1e46b02461b6e6337d05033ef8`, kept
outside Git at `C:\Users\developer\tmp\`. One page, produced by
`Microsoft: Print To PDF`.

### 2.1 Extraction method and its limits

`pdftoppm` is absent in this environment, so the drawing could not be
rendered. Xpdf's `pdftotext` is present but has no `-bbox`. Both readings below
were therefore taken and required to agree:

- `pdftotext -layout`, which decodes the subset fonts and preserves columns.
- `tools/pdf-schematic-text/pdf_text_positions.py`, which recovers
  `(x, y, text)` from the content stream and decodes each string through its
  font's own `/ToUnicode` CMap. It was written for this question and retained
  so the route below can be re-run rather than taken on trust.

Requiring both to agree is what makes the mapping below evidence rather than a
column guess. The positional reading also supplies its own invariant. Across
thirteen module-pin/net-label pairs the label sits above its pin's name by one
of exactly two offsets — `+3.84 pt` for the nine labels drawn in the `x≈511`
and `x≈523` columns, `+4.20 pt` for the four drawn at `x≈480` and `x≈577` —
and the grouping tracks the label's column, that is, which font instance drew
it. In the debug-interface block the seven wire labels sit at a constant
`+0.48 pt` above their `TP` designator, while the two power-symbol labels
(`WLAN_3.3V`, `GND`) sit at `-2.64 pt`, as power symbols are drawn below their
net. A mis-associated label would break the grouping, so the offsets are a
check and not merely a description. They are not a single constant, and stating
them as one would have been the overstatement Section 14.2 warns about.

**What this method cannot see.** It does not follow wires, junctions, or
buses, so every association below is proximity plus a constant offset, not
traced connectivity. It cannot see anything geometric: no pad positions, no
silkscreen, no indication of which side of the AddOn a test point is on, and
no proof that any test point is physically reachable on this assembled
sandwich. 51 of 344 recovered strings decoded to replacement characters; all
51 sit at the degenerate positions `(0,0)` or `(3,6)` and are embedded font
programs picked up by the stream scan, not page text. Nothing below depends on
them.

### 2.2 The AddOn has a Debug Interface

The schematic carries a block labelled `Debug Interface` with nine test
points. Both readings agree on all nine:

| TP | Net | Reaches | Series part |
| --- | --- | --- | --- |
| TP1 | `WLAN_3.3V` | the C6's 3.3 V rail | — |
| **TP2** | `RF_C6_TXD` | module `TXD0` | **R8, 499 Ω** |
| **TP3** | `RF_C6_RXD` | module `RXD0` | none |
| TP4 | `RF_C6_RST` | module `EN` | R16, 0 Ω from `RST` |
| TP5 | `RF_C6_IO9` | module `IO9` (strap) | none; R9 4.7 kΩ pull-up |
| TP6 | `GND` | ground | — |
| TP7 | `RF_C6_USB_N` | module `IO12` | R2, 22 Ω |
| TP8 | `RF_C6_USB_P` | module `IO13` | R3, 22 Ω |
| TP9 | `RF_C6_IO8` | module `IO8` (strap) | none; R7 4.7 kΩ pull-up |

That TP7/TP8 are the C6's native USB pair is the schematic's own claim, carried
in the net names `RF_C6_USB_N` and `RF_C6_USB_P`; it was not separately
confirmed against an ESP-IDF header, because the locked IDF checkout is sparse
and does not contain the relevant `soc` definitions. Nothing below depends on
it, since Option 3 is rejected.

`TXD0` / `RXD0` on an ESP32-C6 module are GPIO16 / GPIO17. That is confirmed
independently of the schematic: ESP-IDF v5.5.3
`components/soc/esp32c6/include/soc/uart_pins.h` defines
`U0TXD_GPIO_NUM 16` and `U0RXD_GPIO_NUM 17`.

The NG slave sets **no** `CONFIG_ESP_CONSOLE_*` key in either
`sdkconfig.defaults` or `sdkconfig.defaults.esp32c6`, so IDF's own defaults
apply: `ESP_CONSOLE_UART_DEFAULT` (UART0) at
`ESP_CONSOLE_UART_BAUDRATE` **115200**, both read from
`components/esp_system/Kconfig` in the locked IDF checkout rather than from
memory. The ROM and second-stage bootloaders print on the same UART at the
same rate.

So **TP2 + TP6 is the console**, and TP2 already has a 499 Ω series resistor
between it and the C6 pin.

### 2.3 GPIO42 is a power gate, not only a reset

This was not previously recorded anywhere in the repository, and it changes
what a "C6 reset" means.

```
J1 "RST"  ──┬── U1 pin 1 (EN)   U1 = SY8089AAAC buck ── L4 ── WLAN_3.3V (600 mA)
   (from    │
  P4 GPIO42)├── R15 100 kΩ ── toward the C20/C21 ground rail
            │
            └── R16 0 Ω ── RF_C6_RST ──┬── C6 module EN
                                       ├── R11 10 kΩ ── WLAN_3.3V
                                       └── C1 1 µF ── GND
```

Verified positionally: the net label `RST` sits on U1 pin 1, whose pin name is
`EN`; `RST` and `RF_C6_RST` sit on the same row on opposite sides of R16
(`0R/1%`); `RF_C6_RST` also sits on the module's `EN` pin and on TP4.

Consequences:

- Driving GPIO42 low does not hold the C6 in reset. It **disables the
  regulator that powers the C6**, so the C6 loses its 3.3 V rail entirely.
- R11/C1 give the C6's own `EN` roughly a 10 ms release after the rail rises —
  deliberate power sequencing, and the reason a reset pulse cannot be short.
- R15's far terminal is not resolvable from text positions. It sits in the same
  row as the C20/C21 input capacitors and above the same ground symbol, which
  is consistent with a 100 kΩ **pull-down** on `EN`; that is also the ordinary
  SY8089 arrangement. If it is a pull-down, then with GPIO42 released or
  floating the rail is **off and latched off**, because R11's pull-up is
  referenced to the rail that `EN` gates. This is exactly the kind of claim
  that should be measured rather than derived — see check R1.

This also means the retained resetpin decision has a second justification.
`0002` leaves `resetpin` at `HOST_GPIO_PIN_INVALID` because a live GPIO42
reset the card mid-enumeration; it now also avoids `esp_reset()` cutting the
C6's supply from module init.

### 2.4 Two AddOn signals reach the P4 that the board contract does not record

`board-contract.json`'s `stamp_addon_c6_sdio` records seven signals: reset 42,
clk 43, cmd 44, data 45-48. J1 on the AddOn carries more than that. Its net
labels, read positionally, are:

| J1 side | Nets |
| --- | --- |
| odd pins | `RST`, `SDIO2_CLK`, `SDIO2_CMD`, `SDIO2_D1`, `SDIO2_D0`, `SDIO2_D2`, `SDIO2_D3`, `RF_C6_IO2` |
| even pins | `GND`, `SYS_5V`, `RF_C6_IO9`, `SYS_SCL`, `SYS_SDA`, `GND` |

`RF_C6_IO9` is a C6 **strapping pin** and it is wired to the P4 through J1.
Which P4 GPIO it lands on cannot be determined from anything in this
repository, because the Stamp-P4 module's own B2B pinout is not held here.
This is the Section 14.19 shape: the model cannot express the signal, so no
checker can see it, and no amount of running the existing checks would have
surfaced it. The measurement in check R2 sidesteps the unknown entirely —
it observes the strap's voltage instead of deriving which pin drives it.

### 2.5 A standing repository caveat is now resolved

`m2/README.md` says "CMD and DAT0..3 require pull-ups; the image enables the P4
internal pulls as a diagnostic aid, but that is not a substitute for the
external pull-ups required by the ESP-Hosted electrical contract." The AddOn
provides them. Eight 4.7 kΩ pull-ups to `WLAN_3.3V`, one per row, verified by
the same constant-offset alignment:

| Net | Pull-up | Series |
| --- | --- | --- |
| `RF_C6_IO8` | R7 | — |
| `RF_C6_IO9` | R9 | — |
| `SDIO2_CMD` | R10 | R4, 22 Ω |
| `SDIO2_CLK` | R12 | R6, 22 Ω |
| `SDIO2_D0` | R20 | R5, 22 Ω |
| `SDIO2_D1` | R21 | R17, 22 Ω |
| `SDIO2_D2` | R22 | R18, 22 Ω |
| `SDIO2_D3` | R23 | R19, 22 Ω |

R24 and R25 are marked NC. The C6-side SDIO pins are the fixed slave set:
`CMD=IO18`, `CLK=IO19`, `D0=IO20`, `D1=IO21`, `D2=IO22`, `D3=IO23`.

## 3. Proposed observation route

Ranked by invasiveness, least first.

**Option 1 — DMM on TP1/TP5/TP9 against TP6. No wiring, no firmware, no
connection to any host.** Three probe touches with the board powered by module
USB-C as it is now. This answers "is the C6 even powered" and "are its straps
where they should be" before anything is soldered, and it is the only option
with no failure mode of its own. It cannot see the console.

**Option 2 — one wire from TP2 to a USB-UART RX, ground referenced at TP6.
This is the recommended console.** Read-only by construction: TP3 (`RF_C6_RXD`)
is deliberately left unconnected, so nothing can be injected into the C6. TP2
already carries R8, 499 Ω in series. Requires a USB-UART adapter whose logic
level is 3.3 V, and its ground tied to TP6 with its VCC/3V3 output left
unconnected, since the C6 is powered from the P4 side and a second supply must
not appear on `WLAN_3.3V`.

**Option 3 — TP7/TP8 to a USB host, for the C6's native USB-Serial/JTAG.**
Rejected for this step. The NG build selects no USB console, so
USB-Serial-JTAG would give a JTAG endpoint and no boot log; it also introduces
a second VBUS into a board where simultaneous USB-A and module USB-C power is
already prohibited.

**Precondition for options 2 and 3:** the test points must be physically
reachable on the assembled sandwich. The schematic proves they exist as
electrical nodes; it says nothing about which side of the AddOn PCB they sit
on, their size, or whether the mated stack covers them. That is a visual
check on the actual hardware and it has to happen before any wire is made.

## 4. Exact read-only checks

None of these writes anything, resets anything, or changes the P4 baseline.
`force-pio` stays enabled, `resetpin` stays invalid, the C6 is not reflashed.

### Group R — DMM only, no console, no wiring

| ID | Measure | Expect if healthy | Meaning if not |
| --- | --- | --- | --- |
| R1 | TP1 to TP6, P4 running Linux | ≈3.3 V, steady | 0 V means the C6 is unpowered and every SDIO observation so far came from a slave peripheral on a rail that is now gone; a rail that collapses later means GPIO42 stopped being driven |
| R2 | TP5 to TP6 | ≈3.3 V | low means `IO9` is held down and the C6 booted into download mode, which would explain enumeration succeeding while no NG app runs |
| R3 | TP9 to TP6 | ≈3.3 V | as R2, for the second strap |
| R4 | TP4 to TP6 | ≈3.3 V after the R11/C1 delay | held low means the C6 is in reset |

R1 and R2 together are the cheapest discriminator available and they need no
serial connection at all. Run them first.

### Group C — console capture on TP2, 115200 8N1

One capture, armed before power is applied, then a cold Type-C power cycle so
the ROM bootloader banner is inside the capture window. Each check names the
question from the investigation brief that it answers.

| ID | Question | Evidence to look for | Reading |
| --- | --- | --- | --- |
| C1 | Does the C6 boot normally, or reset/panic? | `ESP-ROM:esp32c6-...` banner, then `boot:` lines from the second-stage bootloader | Repeating banners mean a boot loop; `Guru Meditation` / `rst:0x...` reason codes name the cause; `waiting for download` means a strap put it in download mode, tying back to R2/R3 |
| C2 | Which image actually booted? | `boot: Loaded app from partition at offset 0x...` and the app description line | Proves or refutes Section 1.2's inference directly. An offset of `0x130000`/`0x250000` is an OTA slot; `0x10000` is factory, i.e. the OTA did not take |
| C3 | Does the ESP-Hosted slave application start? | NG's own startup logging and its version string | Absence with a healthy C1/C2 points at a partition-table mismatch or a Wi-Fi library/ROM-patch problem |
| C4 | Does SDIO transport initialization complete? | slave-side SDIO init logging | The host has proved the slave peripheral answers CMD5; this shows whether the *application* attached to it |
| C5 | Is the boot-up event queued? | NG's bootup/event path logging | This is the specific frame the host never received. If the slave says it queued one, the fault is between the slave's queue and the host's `PACKET_LEN`; if it never queued one, the fault is above the transport |
| C6 | Is `OPEN_DATA_PATH` received? | slave-side receipt of the host's command | The host logged `OPEN_DATA_PATH write completed: ret=0` at boot lines 238-239. If the slave never sees it, the host's write is not landing despite returning success |
| C7 | Is packet-length/interrupt state updated? | any slave-side indication that it wrote `PACKET_LEN` or asserted the host interrupt | Pairs directly with the host-side measurement that `INT_RAW`, `INT_ST` and `PACKET_LEN` are all zero |

Capture handling follows the existing rule: raw logs stay outside Git under
`C:\Users\developer\tmp` and are referenced here by SHA-256. Telemetry only —
no credentials.

### What this plan cannot see

- Nothing here observes the SDIO bus itself. If both sides claim to have done
  their part, a logic-analyzer capture on CLK/CMD/DAT1 becomes the next step,
  and only then.
- The console shows the C6 at 400 kHz with `force-pio`, which is the current
  baseline. It says nothing about behaviour at higher clocks or with DMA.
- A DMM reading at one moment is not a guarantee across the whole boot. R1 in
  particular would miss a rail that dips only during the SDIO probe window.
- Option 2 observes TX only. It cannot exercise the slave, issue a command, or
  read a register, by design.
- If the AddOn's test points turn out to be unreachable on the assembled
  stack, none of Group C can run and the plan reduces to Group R.
