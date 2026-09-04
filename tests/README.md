# Firmware port evidence

Test logs belong here only after they have been captured from the assembled
target. Do not add Wi-Fi passwords, SSH private keys, serial captures that
contain credentials, or unverified flash offsets.

Host-side build evidence is also recorded here when it is explicitly marked
as non-flashing and non-acceptance evidence; see
[`m1-build-2026-08-09.md`](m1-build-2026-08-09.md).

The required evidence is tracked by Issue #6:

- M0: ten recoverable flash/reset cycles and archived stock images;
- M1: twenty cold boots to a BusyBox shell over serial;
- M2: C6 SDIO scan, WPA2 association, DHCP/DNS, reconnect, and 30-minute soak;
- M3: key-only SSH for 30 minutes and persistence rules;
- M4: USB-A CDC enumeration and twenty plug/unplug cycles;
- M5: clean-tree rebuild and reviewer handoff.
