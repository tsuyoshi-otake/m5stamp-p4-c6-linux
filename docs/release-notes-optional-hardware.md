# v0.2.1-smp addendum: optional EasyStick carrier

This documentation/hardware addendum accompanies the existing v0.2.1-smp
release. It does not announce a new firmware build or change existing binary
checksums or the release tag.

## Optional hardware: easy USB mass-storage configuration

The EasyStick Stamp-P4 Rev0.15 carrier is now documented as an **optional**
hardware companion. With the release firmware, its USB-A connection exposes
the 256KB `EASYSTICK` configuration drive to a PC without separately wiring
the Stamp-P4's GPIO26/27 Full-Speed USB interface.

- The carrier is not required for Linux boot, Wi-Fi, SSH, or the Stamp-P4
  USB-C serial console. Wi-Fi still requires the C6 AddOn and SDIO connection.
- MIT-licensed schematic PDF, PCB Gerber ZIP, and SHA-256 checksums are
  available in [`board/hardware/`](../board/hardware/README.md).
- Rev0.15 has been manufactured and used successfully by the project owner.
  Original WIP filenames are preserved for artifact identity; see the hardware
  notes for fabrication conditions and assembly limitations.
- Use only one USB power input at a time. USB-C is used for flashing/serial;
  the carrier's USB-A is used for mass storage.
- Safely eject after editing configuration, then reboot normally to persist
  validated changes. This is a configuration drive, not a general-purpose disk.

The firmware binaries are unchanged by this documentation/hardware addition.
The carrier is not included in the firmware download. The Gerber ZIP describes
the bare PCB; Stamp-P4 installation and other assembly are separate steps.

## Validation scope

- Record a real PC-host enumeration, edit, Safe Eject, and reboot/persistence
  check before claiming full acceptance of this workflow. The existing
  [v0.2.1 evidence](../linux/tests/v0.2.1-hardware-validation-2026-09-08.md)
  exercises a simulated eject and the A/B commit path.
- The hardware addition is separate from firmware binaries/checksums.
