# Serial access, troubleshooting and recovery

The README examples use Windows (`COM10`). The same steps work on Linux and
macOS with the device names below.

| OS | Serial port | Example |
|---|---|---|
| Windows | `COM10` | `esptool.py --port COM10 ...` |
| Linux | `/dev/ttyACM0` | `esptool.py --port /dev/ttyACM0 ...` |
| macOS | `/dev/cu.usbmodem*` | `esptool.py --port /dev/cu.usbmodem1101 ...` |

Linux users may need to be in the `dialout` group (`sudo usermod -aG dialout $USER`).

## Opening the console without resetting the board

The ESP USB Serial/JTAG peripheral resets the chip when the host changes
DTR/RTS while opening the port (`rst:0x17 CHIP_USB_UART_RESET`). Open the port
without toggling them:

```bash
# Linux / macOS
picocom --noreset -b 115200 /dev/ttyACM0
stty -F /dev/ttyACM0 -hupcl          # Linux; use `stty -f` on macOS
```

```python
# pyserial: set the lines before the port is opened
import serial
s = serial.Serial()
s.port = "/dev/ttyACM0"
s.baudrate = 115200
s.dtr = False
s.rts = False
s.open()
```

## Terminal echo

If the host tty keeps echo enabled, the board's output is echoed back into the
board and produces bogus errors such as `sh: can't execute 'ES_OVERLAY'`.
Use a raw, non-echo tty:

```bash
stty -F /dev/ttyACM0 raw -echo       # `stty -f` on macOS
```

## Troubleshooting

**SoftAP (`m5-XXXX`) is not visible.** The SSID is per device; read `ssid=` in
`credentials.txt` on the USB drive. Watch the console for `M3-lab: SoftAP started`
and `M3-lab: SoftAP channel`.
If `wlan0 did not appear`, the SDIO link to the C6 failed (check the C6
firmware and the `ES_SDIO_*` lines). If `wifi_mode=sta` is set in `config.txt`,
or `wpa_supplicant.conf` has no `mode=2`, the board starts as a station instead.

**Station mode does not connect.** Check `wpa_supplicant.conf` on the USB drive
(PSK of 8-63 characters, correct SSID). If `wpa_supplicant` rejects the file the
board falls back to the built-in AP configuration (SSID `m5`, passphrase
`m5stamp-p4-c6`) and logs
`wpa_supplicant failed with ...`. If DHCP fails the status is `DHCP_FAILED`.
If the board never associates, `/run/status.txt` and the console
(`wlan0 did not associate (...)`) say `SSID_NOT_FOUND` when the network was not
seen in any scan (wrong SSID, out of range, 5 GHz only) and `AUTH_FAILED` when
it was seen but the connection failed (usually a wrong PSK).

**Cannot log in over SSH.** The SSH server runs from inetd with at most three
simultaneous connections; wait for stale connections to close or reboot. Root
login is disabled; use the `m5` user with `password=` from `credentials.txt`
(`/tmp/credentials.txt` as root on the serial console). Look for
`ES_CREDENTIALS` lines: if the password was rejected or `cryptpw` failed, the
image has no built-in password, so password login for `m5` stays disabled
(SSH keys from `authorized_keys` still work); fix `password=` and reboot. A power cycle before the first
reboot regenerates the credentials; reboot once after first boot to save them.

**The USB drive does not appear.** Look for `ES_USB_GADGET OK` on the console.
Configuration edits are applied only after the host's safe-eject action.

**Need more kernel output.** Add `debug=1` to `config.txt`, safe-eject and
reboot: S40network then leaves all kernel messages on the console instead of
lowering it to warnings. Expect slower Wi-Fi/SSH while it is on, and remove it
afterwards. As root, `cat /run/messages.0 /run/messages`
shows syslog (Dropbear logins, DHCP), capped at 64 KB, and `sysinfo` a
status summary.

**Settings broke Wi-Fi or SSH.** Put an empty `factory-reset` file on the USB
drive and reboot to restore the defaults (see the README).

**Boot loop / `CRASH_CAPSULE`.** The boot-shim prints
`CRASH_CAPSULE VALID seq=... reason=... epc=... ra=...` and stores the record
in the `crashlog` partition. `reason=1` is a watchdog reset. Resolve `epc` and
`ra` with the `System.map` of the exact kernel `Image` that is flashed.

The capsule holds only registers. For the kernel log of the crashed boot,
m3-lab images reserve pstore (ramoops) by default (`PSTORE=0` leaves
it out). It keeps an oops
or panic and the console tail in 64 KB of reserved PSRAM; after the reset,
as root, read `/sys/fs/pstore/` (`ES_PSTORE OK: N record(s)` on the console).
On hardware the record survived both a sysrq panic and a `panic=0` hang that
the watchdog reset (`linux/tests/m3-lab-hardware-validation-2026-10-14.md`,
"#26 pstore"). A power cycle clears PSRAM, so read it before unplugging.

## Backup and recovery

Back up the whole flash before experimenting:

```bash
esptool.py --port /dev/ttyACM0 read_flash 0 0x1000000 backup-16mb.bin
```

To restore, put the chip in ROM download mode (hold BOOT, tap RESET) and write
the image back:

```bash
esptool.py --port /dev/ttyACM0 --baud 460800 write_flash 0x0 backup-16mb.bin
```
