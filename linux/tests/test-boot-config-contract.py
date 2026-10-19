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

    s05 = read("linux/m3-lab/rootfs-overlay/etc/init.d/S05easystick-tmpfs")
    build = read("linux/build-m1.sh")

    assert "Reboot Persistence" not in readme
    assert "Eject / Safely Remove" in readme
    assert "Saved to flash" in readme
    assert "inactive flash slot" in readme
    assert "No eject or reboot is needed" in readme

    assert "\t\tdevice-owned)\n\t\t\tIMPORT_MODE=eject\n" in bootsync
    assert "mount -t vfat -o ro,loop" in bootsync
    assert 'BOOT_COMMIT="/usr/sbin/easystick-boot-commit"' in bootsync
    assert '"$BOOT_COMMIT" "$SNAP_IMG"' in bootsync
    assert "forced_eject" not in bootsync
    # An edit is saved once the host stops writing, without an eject: the
    # 1 s poll compares mtimes with shell builtins and execs nothing.
    assert '[ "$BOOT_IMG" -nt "$SEEN_REF" ]' in bootsync
    assert 'touch -r "$BOOT_IMG" "$SEEN_REF"' in bootsync
    assert "touch -r /tmp/boot.img /run/.bootsync-seen" in s05
    # The kernel writes flash (patch 0072); bootsync reboots only when it
    # could not (exit 2), or for a factory reset.
    assert 'FLASH_HASH_FILE="/run/.boot-flash-md5"' in bootsync
    assert 'if [ "$rc" -eq 2 ] && auto_reboot_enabled; then' in bootsync
    assert "\n\t\treboot\n" in bootsync
    assert "/run/.boot-flash-md5" in s05
    assert '"/sys/kernel/easystick/boot_commit"' in commit_helper
    assert "#define EXIT_STAGED_ONLY             2" in commit_helper
    assert "patches+=(0072-easystick-esp32p4-bootflash.patch)" in build
    assert '"$patch_name" == 0072-easystick-esp32p4-bootflash.patch ||' in build
    # The everyday commands come from busybox-tools, not the main BusyBox.
    assert 'BR2_PACKAGE_BUSYBOX_TOOLS y' in build
    assert '"BR2_PACKAGE_BUSYBOX_TOOLS=y"; do' in build
    assert "tools_bytes < 480 * 1024" in build
    tools_config = read(
        "linux/buildroot-external/package/busybox-tools/busybox-tools.config"
    )
    for applet in ("AWK", "LESS", "TOP", "FREE", "TAR", "WGET", "IP", "PGREP"):
        assert f"CONFIG_{applet}=y\n" in tools_config, applet
    assert "# CONFIG_SHELL_ASH is not set" in tools_config
    # Pico-style autorun (#34): S05 stages the drive's program, bootsync
    # starts it and restarts it only when the copy changed, always as m5.
    autorun = read("linux/m3-lab/rootfs-overlay/usr/lib/easystick/autorun.sh")
    assert "CONFIG_CHPST=y\n" in tools_config
    assert ". /usr/lib/easystick/autorun.sh" in bootsync
    assert ". /usr/lib/easystick/autorun.sh" in s05
    assert "if autorun_stage /boot; then" in s05
    assert 'if ! autorun_stage "$MNT_POINT"; then' in bootsync
    assert bootsync.count("autorun_apply") == 2
    assert "autorun_trim_log" in bootsync
    assert 'chpst -u "$AUTORUN_USER" -P -n 10' in autorun
    assert 'chpst -u "$AUTORUN_USER" kill' in autorun
    assert "AUTORUN_USER=m5\n" in autorun
    assert "for n in main.py main.sh main; do" in autorun
    # hush + uClibc glob() leave a glob unexpanded after a quoted "-".
    assert "for f in $AUTORUN_NEW/*.sh; do" in autorun
    assert '"$AUTORUN_NEW"/*' not in autorun
    assert '[ "$magic" != bFLT ]' in autorun
    assert "v=${v%%[!A-Za-z0-9._-]*}" in autorun
    # The main BusyBox find has no -type; a failed find hashed nothing, so
    # every program looked the same and a saved change never restarted it.
    assert "find -type" not in autorun and " -type " not in autorun
    assert "/usr/sbin/easystick-autorun" in read("linux/m3-lab/post-build.sh")
    # main.py needs time, socket, json, ...: the "minimal" MicroPython had
    # only os and sys.  The bigger build must still fit the 512 KB block.
    mpy = read("linux/buildroot-external/package/micropython-nommu/micropython-nommu.mk")
    assert "VARIANT=nommu" in mpy and "build-nommu/micropython" in mpy
    variant = read("linux/buildroot-external/package/micropython-nommu/variant/mpconfigvariant.h")
    assert "MICROPY_CONFIG_ROM_LEVEL_EXTRA_FEATURES" in variant
    assert "mpy_bss_end + mpy_stack < 496 * 1024" in build
    # Editing autorun= must not reload Wi-Fi; the README build line needs -fPIC.
    assert "net_settings_changed \"$STAGE/$name\" \"/tmp/$name\"" in bootsync
    assert "riscv32-linux-gcc -Os -fPIC -Wl,-elf2flt" in readme
    # bootsync runs under umask 077; a network reload from it wrote resolv.conf
    # 0600, so m5 could not resolve names.  Its children get a login umask.
    assert "( umask 022; exec /etc/init.d/S40network reload )" in bootsync
    assert "/etc/init.d/S40network reload >/dev/null 2>&1 &" not in bootsync
    assert "\t\tumask 022\n" in autorun
    # The login message must not tell users to eject and reboot any more.
    motd = read("linux/m3-lab/rootfs-overlay/etc/motd")
    assert "No eject or reboot is needed" in motd
    assert "busybox-tools --list" in motd
    assert "easystick-autorun status" in motd
    assert "### 5. Autorun from the USB drive (Pico-style)" in readme
    assert max(len(line) for line in motd.splitlines()) <= 78
    # A reused Buildroot output must not keep a stale boot-commit helper.
    assert 'for pkg_dir in "$external_root"/package/*/; do' in build
    assert '"${pkg_name}-dirclean"' in build
    assert "CONFIG_ESP32P4_BOOTFLASH=y" in read("linux/linux.config")
    # Password and keys are applied live by bootsync and at boot by S05.
    assert ". /usr/lib/easystick/credentials.sh" in bootsync
    assert ". /usr/lib/easystick/credentials.sh" in s05

    # #37: root over SSH is key-only (-g kept, -w gone); a setuid su-only
    # BusyBox and root_password= cover su.  The image shadow stays locked.
    inetd = read("linux/m3-lab/rootfs-overlay/etc/inetd.conf")
    assert "dropbear -i -g -r" in inetd
    assert " -w " not in inetd
    credentials = read(
        "linux/m3-lab/rootfs-overlay/usr/lib/easystick/credentials.sh"
    )
    assert "install_root_keys" in credentials
    assert "chmod 700" in credentials and "chmod 600" in credentials
    assert "es_install_keys \"$1\" root /root 0:0" in credentials
    assert "ES_SSH_KEYS FAIL" in credentials
    assert "mke2fs -q -m 2 " in s05
    assert "apply_root_password" in credentials
    assert "root_password" in credentials
    assert "install_root_keys /boot/root_authorized_keys" in s05
    assert "apply_root_password /boot/credentials.txt" in s05
    assert "root_password=$rpw" in s05
    assert "install_root_keys \"$STAGE/root_authorized_keys\"" in bootsync
    assert "root_authorized_keys" in bootsync
    assert "apply_root_password \"$STAGE/credentials.txt\"" in bootsync
    # A key file deleted from the drive revokes only the keys it installed.
    assert ": > /run/.es-keys-m5" in credentials
    assert ": > /run/.es-keys-root" in credentials
    assert '[ -e "/run/.es-keys-$1" ] || return 0' in credentials
    assert "revoke_keys m5 /home/m5 authorized_keys" in bootsync
    assert "revoke_keys root /root root_authorized_keys" in bootsync
    su_pkg = read("linux/buildroot-external/package/busybox-su/busybox-su.mk")
    assert "4755" in su_pkg and "BUSYBOX_SU_PERMISSIONS" in su_pkg
    su_config = read(
        "linux/buildroot-external/package/busybox-su/busybox-su.config"
    )
    assert "CONFIG_SU=y\n" in su_config
    assert "CONFIG_FEATURE_SUID=y\n" in su_config
    assert "CONFIG_FEATURE_SU_SYSLOG=y\n" in su_config
    assert "CONFIG_FEATURE_SHADOWPASSWDS=y\n" in su_config
    su_patch = read(
        "linux/buildroot-external/package/busybox-su/"
        "0001-libbb-a-locked-password-never-matches.patch"
    )
    assert "if (pw_pass[0] == '!' || pw_pass[0] == '*')" in su_patch
    assert "# CONFIG_FEATURE_SU_CHECKS_SHELLS is not set" in su_config
    busybox_fragment = read("linux/m3-lab/busybox.fragment")
    assert "# CONFIG_SU is not set" in busybox_fragment
    assert "BR2_PACKAGE_BUSYBOX_SU" in build
    post_build = read("linux/m3-lab/post-build.sh")
    assert "root:!:" in post_build
    # Login records: S05 creates utmp/wtmp for Dropbear, busybox-tools
    # reads them, and Dropbear rotates wtmp on the /tmp ramdisk.
    assert "for f in /var/run/utmp /var/log/wtmp /var/log/wtmp.1; do" in s05
    assert 'chown root:utmp "$f" && chmod 664 "$f"' in s05
    for opt in ("FEATURE_UTMP", "FEATURE_WTMP", "WHO", "W", "USERS", "LAST"):
        assert f"CONFIG_{opt}=y\n" in tools_config, opt
    assert "usr/bin/who usr/bin/w usr/bin/users usr/bin/last; do" in build
    wtmp_patch = read("linux/m3/dropbear-patches/0011-easystick-wtmp-rotate.patch")
    assert "+#define WTMPX_ROTATE_SIZE (32 * 1024)" in wtmp_patch
    assert "+\t(void)flock(fd, LOCK_EX);\n+\twtmpx_rotate(fd);" in wtmp_patch
    assert '+\tif ((out = open(WTMPX_FILE ".1", O_WRONLY|O_TRUNC, 0)) < 0) {' in wtmp_patch
    assert "rename(" not in wtmp_patch
    assert '"${script_dir}/m3/dropbear-patches/"*.patch' in build
    assert "who w last" in motd
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

    # The kernel writer (0072) must produce what the shim validates.
    bootflash = "\n".join(
        line[1:]
        for line in read(
            "linux/kernel-patches/0072-easystick-esp32p4-bootflash.patch"
        ).splitlines()
        if line.startswith("+")
    )
    for firmware_name, kernel_name in (
        ("BOOT_UPDATE_REQUEST_PA", "BOOT_REQUEST_PA"),
        ("BOOT_IMAGE_BYTES", "BOOT_IMAGE_BYTES"),
        ("BOOT_REQUEST_MAGIC", "BOOT_REQUEST_MAGIC"),
        ("BOOT_REQUEST_VERSION", "BOOT_REQUEST_VERSION"),
        ("BOOT_REQUEST_COMMIT", "BOOT_REQUEST_COMMIT"),
        ("BOOT_META_MAGIC", "BOOT_META_MAGIC"),
        ("BOOT_META_VERSION", "BOOT_META_VERSION"),
        ("BOOT_META_COMMIT", "BOOT_META_COMMIT"),
    ):
        assert define(shim_contract, firmware_name) == define(bootflash, kernel_name)
    assert define(bootflash, "BOOT_IMAGE_PA") == 0x499C0000
    layout = (ROOT / "linux/boot-shim/partitions.csv").read_text(encoding="utf-8")
    for name, offset in (
        ("boot", "0xf40000"),
        ("boot_alt", "0xf80000"),
        ("bootmeta0", "0xfc0000"),
        ("bootmeta1", "0xfc1000"),
    ):
        assert re.search(rf"^{name},.*{offset}", layout, re.MULTILINE | re.IGNORECASE), name
    assert "slot_base[2] = { 0xf40000, 0xf80000 }" in bootflash
    assert "meta_base[2] = { 0xfc0000, 0xfc1000 }" in bootflash

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
