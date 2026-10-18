# Security policy

Please report vulnerabilities privately through GitHub's
"Report a vulnerability" (private security advisory) feature on this
repository instead of opening a public issue.

Known limitations of the current images (see the issue tracker for details):

- Each unit generates its own Wi-Fi PSK and `m5` SSH password on first boot
  (`credentials.txt` on the USB drive, #24). Anyone who can read that drive or
  the flash has them.
- Secure Boot and flash encryption are disabled. Anyone with physical access
  can read the flash (including the boot FAT with Wi-Fi credentials) and write
  arbitrary firmware. [docs/security-hardening.md](docs/security-hardening.md)
  lists what a product build would need; none of it is tested.
- The USB-C serial console (`ttyGS1`) gives an unauthenticated root shell.

## Third-party components

Every upstream source is pinned in `versions.lock.json` (commit, tag object
and, for Linux, the stable tag); `python3 tools/verify-source-lock.py` checks
the lock and the submodules. The network-facing components are:

| Component | Where it is pinned | Watch |
|---|---|---|
| Linux stable (7.2.y) | `linux_base` in `versions.lock.json` | stable releases, `linux-cve-announce` |
| Dropbear, wpa_supplicant, BusyBox, uClibc-ng | the Buildroot commit in `versions.lock.json` | Buildroot LTS point releases, upstream advisories |
| ESP-Hosted NG (P4 driver and C6 firmware) | `esp_hosted_ng` in `versions.lock.json` | Espressif advisories |
| ESP-IDF (boot shim, C6 build) | `esp_idf` in `versions.lock.json` | Espressif advisories |

To take an update:

1. Move the submodule (or the external cache) to the new commit and update
   the matching entry in `versions.lock.json`.
2. Run `python3 tools/verify-source-lock.py` and `bash tests/static-check.sh`.
3. Rebuild (`linux/build-c68.sh --profile m3-lab`); kernel and esp-hosted-ng
   patches must still apply cleanly.
4. Boot the image on hardware and add a `linux/tests/*hardware-validation-*.md`
   record before a release, because the release workflow checks the release
   hashes against those records.
