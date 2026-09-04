# M3 initial setup over USB

The M3 image follows the current Raspberry Pi OS provisioning model: it does
not ship a universal `pi`/`raspberry` credential.  The image contains one
board operator account, `p4`, in a locked state.  The first-boot command asks
the operator to choose its local-console password, Wi-Fi settings, and an SSH
public key.

The password is deliberately local-console-only.  Dropbear is compiled with
password authentication disabled and is started with public-key authentication
and root-login disabled.  The password is still useful for an attached
console and for emergency recovery; it is never a network fallback.

## Console paths

Both connectors use the same command and the same prompts.  Connect only one
power source at a time.

| Path | Electrical role | Software prerequisite | M3 status |
| --- | --- | --- | --- |
| Module USB-C | USB Serial/JTAG/recovery path on GPIO24/GPIO25 | Linux USB-Serial/JTAG console support, or the boot-shim recovery console | Primary setup path once the Linux console driver is ported |
| Carrier USB-A | USB1 FS device on GPIO27/GPIO26; VBUS is input-only | M4 Linux CDC-ACM gadget and login console | Fallback setup path after M4 |

USB-A remains device-only.  It must not source VBUS, enter host mode, or be
connected for power at the same time as module USB-C.  Until the Linux
USB-Serial/JTAG driver is available, use the exposed UART fixture as the
development fallback while preserving USB-C for ROM recovery.

## Procedure

1. Boot an M3 image and mount the board-validated writable `/config` and
   writable `/etc` overlay.  The base squashfs remains read-only.
2. Open a local root shell on USB-C, or on the USB-A CDC-ACM port once M4 is
   enabled.  Do not run provisioning through SSH or another network service.
3. Run:

   ```sh
   /usr/sbin/easystick-firstboot
   ```

4. Set a new password when `passwd` prompts for the `p4` local account.  Do
   not use the retired Raspberry Pi `raspberry` password.
5. Enter the Wi-Fi SSID and passphrase when prompted.  The passphrase is
   echoed off and is stored only as mode-0600 data under `/config`.
6. Paste one OpenSSH public-key line.  The corresponding private key stays on
   the administrator's computer and is never copied to the board or checked
   into Git.
7. Reboot after the C6 SDIO `wlan0` path is enabled.  SSH then accepts the
   configured key as `p4`; password and root login remain disabled.

To reprovision, run the command again from a local console.  To disable SSH,
stop the service and remove the authorized key and per-device host key from
`/config/easystick/`, then reboot.  Reprovisioning and erasure must be tested
on hardware before M3 is marked complete.

The reference for the credential policy is the
[Raspberry Pi OS first-boot and Imager user customisation guide](https://www.raspberrypi.com/documentation/computers/getting-started.html#os-customisation).
