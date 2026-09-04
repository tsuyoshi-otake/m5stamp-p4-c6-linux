#!/usr/bin/env python3
"""Static checks for the M3 Dropbear/USB provisioning profile."""

from __future__ import annotations

import sys
from pathlib import Path


ROOT = Path(__file__).resolve().parent


def read(name: str) -> str:
    path = ROOT / name
    try:
        return path.read_text(encoding="utf-8")
    except OSError as exc:
        raise SystemExit(f"FAIL: cannot read {path}: {exc}") from exc


def require(text: str, needle: str, label: str) -> None:
    if needle not in text:
        raise SystemExit(f"FAIL: {label} is missing {needle!r}")


def main() -> int:
    fragment = read("buildroot.fragment")
    require(fragment, "BR2_PACKAGE_DROPBEAR=y", "Dropbear server")
    require(fragment, "# BR2_PACKAGE_DROPBEAR_CLIENT is not set", "Dropbear client disablement")
    require(fragment, "BR2_PACKAGE_DROPBEAR_DISABLE_REVERSEDNS=y", "reverse-DNS disablement")
    require(fragment, "BR2_PACKAGE_DROPBEAR_LOCALOPTIONS_FILE=", "Dropbear localoptions path")
    require(fragment, "BR2_ROOTFS_POST_BUILD_SCRIPT=", "M3 post-build hook")

    localoptions = read("dropbear-localoptions.h")
    require(localoptions, "#define DROPBEAR_SVR_PASSWORD_AUTH 0", "compile-time password disablement")
    require(localoptions, "#define DROPBEAR_SVR_PUBKEY_AUTH 1", "public-key authentication")

    inetd = read("rootfs-overlay/etc/inetd.conf")
    require(inetd, "dropbear -i -w -s -r", "inetd key-only/root-disabled command")

    users = read("users.txt")
    require(users, "p4 -1 p4 -1 ! /config/easystick/p4 /bin/sh", "locked p4 account")

    setup = read("initial-setup.md")
    require(setup, "USB-C", "USB-C initial-setup path")
    require(setup, "USB-A", "USB-A initial-setup path")
    require(setup, "/usr/sbin/easystick-firstboot", "first-boot command")
    require(setup, "universal `pi`/`raspberry`", "no universal Raspberry Pi credential")

    for relative in (
        "rootfs-overlay/etc/init.d/S10easystick-config",
        "rootfs-overlay/etc/init.d/S85easystick-ssh",
        "rootfs-overlay/usr/sbin/easystick-firstboot",
    ):
        if not (ROOT / relative).is_file():
            raise SystemExit(f"FAIL: missing runtime file {relative}")

    print("PASS: M3 Dropbear/USB provisioning profile is internally consistent")
    return 0


if __name__ == "__main__":
    sys.exit(main())
