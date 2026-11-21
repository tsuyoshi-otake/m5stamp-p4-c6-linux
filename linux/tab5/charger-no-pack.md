# Tab5 without a battery pack: the charger resets the radio chip

Measured 2026-11-21 on one Tab5 (ST7123 panel) running build B59, powered
from a PC USB port through the serial-console cable, with no usable pack on
J12. Everything below was read from Linux on that unit: the INA226 on the
pack node (`/sys/class/hwmon/hwmon0/in1_input`, about 450 samples per second
from a shell loop), `CHG_STAT` through `/sys/class/power_supply/usb/online`,
the C6 link through `/proc/esp32_sdio_h2c_obs`, and the reset cause the C6
firmware reports in its boot-up event.

## What was seen

The ESP32-C6 kept being power-on reset (`print_reset_reason: POWERON_RESET`,
one boot-up event per firmware boot, nobody pulsing its EN line). In 33
minutes after one boot its lifetimes after each SoftAP start were at most
32, 7, 13, 7, 25-43, 10-14, 15, 10, 20, 247 and 54 s. Each death takes Wi-Fi
and Bluetooth with it until the link is re-initialised.

Ruled out first, each by its own measurement:

- SDIO traffic alone (a load test did not kill the link);
- a floating EN line (P4 GPIO15 is an enabled output, driven high);
- the C6 rail switch (expander 0x44 `WLAN_PWR_EN` is driven high);
- a firmware restart or watchdog (those report other reset causes);
- a longer EN pulse from Linux (four deaths in 330 s after a 1 s pulse);
- the backlight level (same behaviour at 30/1024 and at 307/1024).

## The pack node

| State | Pack node | `CHG_STAT` | C6 link |
| --- | --- | --- | --- |
| Charger active, Wi-Fi off, BLE passive scan | steady 8376-8378 mV (3000 samples) | toggles, 0.67 s high / 0.67 s low | alive for the whole 240 s |
| Charger active, SoftAP on | falls from 8.38 V to 5.2-6.9 V about every 105 ms (once per beacon), recovers in 30-70 ms; at times rests near 3.9 V for tens of seconds | high or toggling | dead 15 s after Wi-Fi came up, and again after every recovery |
| Charger stopped by itself, SoftAP on | about 1.9 V, falling 0.35 mV/s | low | alive for 6.6 min, until the test rebooted the board |

The INA226 shunt read 0 mA in every state: no current into or out of a
pack. With the charger off the node sits at 1.8-1.9 V, which no 2S pack
does. So there was no pack behind the node, and the 8.4 V was the charger
alone holding an empty node.

## Charge enable, switched by hand

`CHG_EN` is expander 0x44 P7 (output register 5: 0x89 = on, 0x09 = off).
SoftAP up and the BLE keyboard daemon running throughout:

| `CHG_EN` | Watched for | C6 link | Pack node |
| --- | --- | --- | --- |
| high, as booted (the charger ran from 35 s to 76 s of uptime, then stopped by itself) | 156 s | alive | 1.7-8.4 V |
| low | 157 s | alive | 1823-1830 mV, flat |
| high | | dead within about 8 s of the write | 6.3 V and moving |
| low | 63 s | alive | 1898-1911 mV, flat |
| high | | dead about 12 s after the write | 5.07-8.38 V |
| low | 63 s | alive | 1875-1879 mV, flat |

Charger off: 283 s without a single reset. Charger switched on: the C6 was
reset both times. Not every charge attempt kills it -- the 40 s attempt
right after that boot and one 5 s attempt during the timing measurement
below did not -- but every reset seen that day fell into a period with the
charger active.

Two timings from the same session:

- After `CHG_EN` goes high the charger does not start at once: the node was
  still at 1.86 V after 1 s and after 2 s, and at 7.58 V after 5 s.
- After `CHG_EN` goes low from 7.58 V the node is below 6 V within 30 ms,
  below 3 V within 60 ms, below 2.5 V at 130 ms, below 2.0 V at 310 ms, and
  at 1.82 V after 7 s.

## What is not measured

- The 5 V and 3.3 V rails themselves. Nothing on the board reports them, so
  how the charger's activity reaches the C6's supply is an inference from
  the pack node: the node dipping at every beacon says the system draws
  from it while it is above the 5 V converter's input range, and the C6
  reporting a power-on reset says its supply collapsed.
- A unit with a healthy pack. None was available. A pack holds the node, so
  none of this should happen there, and M5Stack's own firmware enables the
  charger unconditionally -- but it was not seen here.
- Other USB supplies. The unit was only ever on that one PC port.

## What the image does about it

- **Boot shim** (`linux/boot-shim/main/tab5_board.c`, `charger_decide()`):
  the charger stays off through the expander init. After the C6 reset wait,
  1.5 s later, the shim reads the pack node: below 4 V means no pack and
  `CHG_EN` stays low; 4 V or more means a pack and the charger goes on as
  in the stock image. If the INA226 does not answer, the charger goes on.
- **`charger=` in `config.txt`** (`auto`, `on`, `off`; `m5-config`, Power
  menu): `auto` is the default and is the shim's decision. `on` forces the
  charger at boot and on a live save; `off` keeps it off. A live save of
  `auto` runs the same test from Linux (charger off, one second, 4 V).
- `m5-config`'s Status page says `battery: none` when the battery supply
  reports no pack, instead of a percentage.

## Checked on the image with the fix

Same unit, same USB port, builds B60, B61 and B62, 2026-11-21:

- The boot shim logs `no battery pack (pack node 1806 mV with the charger
  off): CHG_EN stays low` (1831 mV on B61); Linux then finds output
  register 5 at 0x09 and the node at 1.88 V.
- SoftAP up and the BLE keyboard daemon running from boot: no C6 reset in
  the first 590 s of one boot, in 1730 s of the next and in the 4140 s
  B61 ran (one boot-up event each, the one of the boot itself). In the
  three boots where it was looked for, the bonded keyboard reconnected
  by itself 23 s, 132 s and 177 s after boot.
- `charger=on` saved through `m5-config` took the register to 0x89 (set
  back by hand at once); removing the key took the live `auto` path and
  left it at 0x09. The live test takes 1.1 s, and 1.3 s with `sleep` made
  unavailable.

A live save is the tightest moment for memory on this board: the menu,
bootsyncd and S40network's apply are all up, and the kernel's user reserve
turns away the next 480 KB program ("refused by user reserve" in the
kernel log). What was turned away, in three rounds of saves (`off`,
`auto`, `on`, key removed):

| Build, helper, uptime | Refusals | Who lost |
| --- | --- | --- |
| B60, helper with `sed` and three `$( )`, 630-830 s | 20 in 4 saves | first 10: `m5-config`'s own cleanup (`rm`, `umount`) and the commands typed right after it |
| B60, helper with builtins only, 400-460 s | 2 in 4 saves | not read out before the next reboot |
| B61, helper with builtins only, 100-140 s | 7 in 3 saves | the same two groups as in the first round |

Of the 27 refusals of the first and the third round, 17 were read out by
name (the first 10 and all 7); the apply itself -- S40network and the
helper -- is not among them. It starts first. So the builtin-only helper
does not end the refusals; the uptimes differ and so does the pool. What
it does is take its own three extra programs out of that moment, and keep
the one-second wait of the `auto` test when `sleep` is the program that
is refused.

What ended them is `m5-config set` waiting for the apply before it
returns (bootsyncd and S40network count the saves they have dealt with in
`/run/.netapply.done`). On B62, 110-147 s after boot -- the condition of
the third round above -- four saves (`off`, `auto`, `auto` again, key
removed), each followed at once by other commands, left no refusal and
no stale lock directory; a `set` took 6.6-8.5 s. A save from the menu
never had the problem: the menu stays up and starts nothing next.

The one case `auto` cannot see: a pack whose protection circuit has opened
after a deep discharge reads 0 V until a charger wakes it. With such a pack
the charger now stays off until `charger=on` is set. This path is untested
for the same reason as above: no pack on the measured unit.
