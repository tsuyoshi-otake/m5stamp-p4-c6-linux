# Optional EasyStick USB mass-storage carrier

EasyStick Stamp-P4 Rev0.15 is an optional carrier that makes the Linux
firmware's USB mass-storage configuration drive easy to connect to a PC.
It routes the Stamp-P4's Full-Speed USB signals (GPIO26 D− / GPIO27 D+)
to USB-A with power and ESD protection, and provides BOOT/RESET switches.

The carrier is not required for Linux boot, Wi-Fi, SSH, or the Stamp-P4 USB-C
serial console. Wi-Fi still requires the Stamp-AddOn C6 and its SDIO connection.
The USB-C flashing/serial port is separate from this USB-A mass-storage port.
With this firmware, USB-A is a device-only connection to a PC; it does not
provide USB-host support for plugging storage devices into the carrier.

## Design downloads

- [Schematic PDF](rev0.15-wip/EasyStick_StampP4_Rev0.15_WIP_Schematic.pdf)
- [PCB Gerber ZIP](rev0.15-wip/EasyStick_StampP4_Rev0.15_WIP_Gerber.zip)
- [SHA-256 checksums](rev0.15-wip/SHA256SUMS.txt)

Source: `easy-stick/projects/easystick-stamp-p4/deliverables/rev0.15-wip/`,
imported unchanged on 2026-09-09 from `schematic/pdf/` and `pcb/`.

## Revision status and reproduction

The project owner reports that Rev0.15 has been manufactured and used
successfully, including USB mass storage. A static circuit/PCB review on
2026-09-09 found no clear blocker to reproducing this Gerber ZIP under the
same fabrication conditions. This is not a certification or a substitute for
the chosen fabricator's final CAM/DFM review and assembly checks.

The original filenames and PDF still say **WIP — NOT FOR MANUFACTURING**.
Those historical labels are retained to preserve the exact reviewed artifacts
and checksums; they predate the owner's report of successful manufacture/use.

- Fabrication: 2-layer FR-4, 53.0 × 26.3mm, 1.6mm thick, ENIG, plated
  castellated half-holes on **two edges**. Request production-file confirmation.
- Assembly: the original BOM/CPL excludes **U1 (Stamp-P4)** for later manual
  installation. The downloads here are a schematic and bare-PCB Gerbers,
  **not a complete turnkey PCBA order package**.
- CAM note: `Drill_PTH_Through.DRL` and `Drill_PTH_Through_Via.DRL` repeat
  the same 81 via locations. Keep the reviewed ZIP intact and ask another
  fabricator to confirm how it handles the overlapping drill data.
- USB-A and Stamp-P4 USB-C must **not be powered simultaneously**; dual-input
  backfeed protection has not been verified.

## Using the optional carrier

1. Flash the Linux release through the Stamp-P4 USB-C port.
2. Disconnect USB-C, then connect the carrier through USB-A to the PC.
3. Edit the files on the 256KB `EASYSTICK` configuration drive and safely eject
   it. Wait for validation/re-attachment before reconnecting or removing power.
4. Reboot normally to commit validated settings to flash. Removing power before
   that reboot loses staged changes.

See the [main Quick Start](../../README.md#3-connect--use) for the complete
safe-eject, status, and persistence procedure. The drive is for configuration;
it does not provide general-purpose access to the Linux root filesystem.

Recorded firmware validation covers a simulated SCSI eject and A/B flash
commit, not the complete PC operating-system Safe Eject path. See the
[v0.2.1 test record](../../linux/tests/v0.2.1-hardware-validation-2026-09-08.md).
Real-host enumeration, editing, Safe Eject, and persistence after reboot should
be recorded before claiming end-to-end release acceptance for that workflow.

## License

EasyStick schematics and Gerber files here are licensed under the
[MIT License](LICENSE). This license applies to these hardware design files;
it does not change the licenses of software elsewhere in the repository or
of third-party reference materials.
