#!/usr/bin/env python3
"""Static fail-closed checks for M3-lab USB ownership and A/B durability."""

from pathlib import Path
import re


ROOT = Path(__file__).resolve().parents[2]


def read(relative: str) -> str:
    return (ROOT / relative).read_text(encoding="utf-8")


def main() -> None:
    readme = read("README.md")
    bootsync = read(
        "linux/m3-lab/rootfs-overlay/usr/sbin/easystick-bootsync"
    )
    updater = read(
        "linux/m3-lab/rootfs-overlay/usr/sbin/easystick-update-status"
    )
    network = read("linux/m3-lab/rootfs-overlay/etc/init.d/S40network")
    lun = read("linux/m3-lab/rootfs-overlay/usr/sbin/easystick-usb-lun")
    shim = read("linux/boot-shim/main/main.c")
    shim_contract = read("linux/boot-shim/main/boot_ab.h")
    commit_helper = read(
        "linux/buildroot-external/package/easystick-boot-commit/src/"
        "easystick-boot-commit.c"
    )
    fragment = read("linux/m3-lab/kernel.config.fragment")

    assert "Reboot Persistence" not in readme
    assert "host-issued SCSI eject" in readme
    assert "boot update committed" in readme
    assert "inactive flash slot" in readme

    assert 'LUN_STATE" = "device-owned"' in bootsync
    assert "mount -t vfat -o ro,loop" in bootsync
    assert 'BOOT_COMMIT="/usr/sbin/easystick-boot-commit"' in bootsync
    assert '"$BOOT_COMMIT" "$SNAP_IMG"' in bootsync
    assert "forced_eject" not in bootsync
    assert "mount -t vfat" not in updater
    assert "BOOT_IMG" not in updater
    assert "boot-snap.img" not in network

    assert "*/gadget/lun*/file" in lun
    assert '"$dir/forced_eject"' in lun
    assert "> \"$dir/forced_eject\"" not in lun
    assert "flash is authoritative" in shim

    def define(source: str, name: str) -> int:
        match = re.search(
            rf"^#define\s+{name}\s+(?:UINT32_C\()?\s*(0x[0-9a-fA-F]+|[0-9]+)",
            source,
            re.MULTILINE,
        )
        assert match, f"missing {name}"
        return int(match.group(1), 0)

    shared = (
        ("BOOT_UPDATE_REQUEST_PA", "EASYSTICK_PSRAM_REQUEST_PHYS"),
        ("BOOT_IMAGE_BYTES", "EASYSTICK_IMAGE_BYTES"),
        ("BOOT_REQUEST_MAGIC", "EASYSTICK_REQUEST_MAGIC"),
        ("BOOT_REQUEST_VERSION", "EASYSTICK_REQUEST_VERSION"),
        ("BOOT_REQUEST_COMMIT", "EASYSTICK_REQUEST_COMMIT"),
    )
    for firmware_name, linux_name in shared:
        assert define(shim_contract, firmware_name) == define(commit_helper, linux_name)
    assert "#define BOOT_LOAD_PA 0x499c0000u" in shim
    assert define(commit_helper, "EASYSTICK_PSRAM_IMAGE_PHYS") == 0x499C0000
    assert "sizeof(struct boot_update_request) == 20" in shim
    assert "sizeof(struct easystick_commit_request) == 20" in commit_helper
    assert "request->commit = EASYSTICK_REQUEST_COMMIT" in commit_helper
    assert "write_boot_metadata" in shim
    assert "esp_partition_erase_range(slots[inactive_slot]" in shim
    assert "partition_crc32(slots[inactive_slot]" in shim

    for expected in (
        "CONFIG_USB_GADGET=y",
        "CONFIG_USB_F_MASS_STORAGE=m",
        "CONFIG_USB_MASS_STORAGE=m",
        "CONFIG_BLK_DEV_LOOP=y",
        "CONFIG_VFAT_FS=y",
    ):
        assert expected in fragment

    print("PASS: boot configuration ownership and durability contract")


if __name__ == "__main__":
    main()
