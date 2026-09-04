# M3: initial setup and SSH

After DHCP succeeds, `S41timesync` performs a best-effort one-shot BusyBox
NTP synchronization against `pool.ntp.org` and records the result in
`/var/log/ntp-sync.log`. Time synchronization is not allowed to block local
console recovery when Wi-Fi or DNS is unavailable.

M3 adds a small Buildroot Dropbear server to the RV32 NOMMU image.  Buildroot
already selects the server; this profile adds the NOMMU-specific BusyBox
`inetd` front-end, compile-time password-authentication disablement, and
per-device Ed25519 host keys.  The server is started only when all of the
following are true:

- the board-specific writable `/config` mount is present;
- `/config/easystick/p4/.ssh/authorized_keys` contains an OpenSSH public key;
- `/config/easystick/dropbear/dropbear_ed25519_host_key` exists (generated on
  first provisioning when absent).

This is the production profile specification, not a flashable image.  The
current M1
kernel exposes only a read-only MTD-ROM rootfs; the writable MTD/overlay and
the C6 SDIO network path must be implemented and tested before M3 can be
enabled on hardware.

The build entry point accepts `--profile m3` and includes this profile for
static/runtime validation, but it deliberately leaves SSH disabled until the
writable config and key files exist.  `../m3-lab/` is a separate, temporary
password-enabled image for the first private-network handshake; do not use it
as the production configuration.

## Build profile

Build the M1 defconfig first, then merge `buildroot.fragment` into the same
Buildroot output directory and run `olddefconfig`:

```bash
make -C "$BUILDROOT" O="$OUT" \
  BR2_EXTERNAL="$REPO/projects/easystick-stamp-p4/firmware/linux/buildroot-external" \
  easystick_stamp_p4_defconfig
cat "$REPO/projects/easystick-stamp-p4/firmware/linux/m3/buildroot.fragment" >> "$OUT/.config"
make -C "$BUILDROOT" O="$OUT" \
  BR2_EXTERNAL="$REPO/projects/easystick-stamp-p4/firmware/linux/buildroot-external" \
  olddefconfig
make -C "$BUILDROOT" O="$OUT" \
  BR2_EXTERNAL="$REPO/projects/easystick-stamp-p4/firmware/linux/buildroot-external"
```

Run the profile-only check before starting a build:

```bash
python3 "$REPO/projects/easystick-stamp-p4/firmware/linux/m3/verify-profile.py"
```

The profile deliberately does not enable Dropbear's client programs.  On a
NOMMU build Buildroot forces Dropbear into inetd mode, so the overlay starts
BusyBox `inetd` rather than the normal MMU `S50dropbear` service.

## USB initial setup

The complete console contract and acceptance notes are in
[`initial-setup.md`](initial-setup.md).  The provisioning command is
`/usr/sbin/easystick-firstboot`.  It sets the `p4` local-console password,
stores Wi-Fi settings, and writes the operator's public key beneath
`/config`; private keys never belong on the target image.  Run the same
command through either of these local paths:

- **USB-C:** the module USB-C/USB Serial-JTAG path is the M0 recovery and
  intended primary setup path.  Native Linux USB-Serial/JTAG support is not
  yet part of the P4 port, so the current development fallback is the
  boot-shim/UART recovery console until that driver is available.
- **USB-A:** after M4 enables the FS-OTG device as a CDC-ACM gadget on
  GPIO27/26, the same command can be run from the connected PC.  USB-A remains
  device-only; this procedure never supplies VBUS and does not enable host or
  OTG mode.

The initial account follows current Raspberry Pi OS provisioning rather than
the retired universal `pi`/`raspberry` login: `p4` is present but locked in
the image, and the password is chosen locally over USB.  SSH still requires a
public key (`-s -w` policy), so the local password is not a network fallback.
Password persistence also requires the eventual writable `/etc` overlay (or
an equivalent password database handoff); `/config` alone is not sufficient.

## Security and acceptance gates

Do not commit Wi-Fi credentials, password hashes, host private keys, or
`authorized_keys` to Git or CI logs.  Before calling M3 complete, verify a
reboot retains the explicitly provisioned data, SSH rejects password and root
logins, a key-authenticated session survives the Wi-Fi soak, and erasing the
config partition disables SSH until reprovisioned.
