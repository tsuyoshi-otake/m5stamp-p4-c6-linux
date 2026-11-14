#!/usr/bin/env python3
"""Create a private station configuration image for the existing boot FAT slot."""

import argparse
import hashlib
import json
import os
from pathlib import Path
import shutil
import string
import subprocess
import tempfile


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--network-json", type=Path, required=True)
    parser.add_argument("--output", type=Path, required=True)
    args = parser.parse_args()
    if args.output.exists():
        parser.error("output already exists; choose a new image path")
    try:
        network = json.loads(args.network_json.read_text())
        ssid = network["ssid"]
        password = network["passphrase"]
        if not isinstance(ssid, str) or not isinstance(password, str):
            raise ValueError("ssid and passphrase must be strings")
        ssid_bytes = ssid.encode("utf-8")
        if not 1 <= len(ssid_bytes) <= 32:
            raise ValueError("SSID must contain 1 to 32 UTF-8 bytes")
        if len(password) == 64 and all(c in string.hexdigits for c in password):
            psk = password.lower()
        elif 8 <= len(password) <= 63 and all(32 <= ord(c) <= 126 for c in password):
            psk = hashlib.pbkdf2_hmac("sha1", password.encode("ascii"), ssid_bytes, 4096, 32).hex()
        else:
            raise ValueError("WPA2 passphrase must be 8 to 63 printable ASCII characters, or a 64-digit hex PSK")
    except (OSError, ValueError, KeyError, TypeError) as exc:
        parser.error(str(exc))
    mkfs = shutil.which("mkfs.vfat") or shutil.which("mkfs.fat")
    mcopy = shutil.which("mcopy")
    if not mkfs or not mcopy:
        parser.error("mkfs.vfat and mcopy are required")
    args.output.parent.mkdir(parents=True, exist_ok=True)
    # A WPA PSK is still a credential. Neither the input nor its derived key
    # belongs in a public rootfs, process arguments, stdout or a build log.
    with tempfile.TemporaryDirectory(prefix="p4-wifi-", dir=args.output.parent) as name:
        directory = Path(name)
        conf = directory / "wpa_supplicant.conf"
        conf.write_text(
            "ctrl_interface=/run/p4-wifi\nupdate_config=0\nnetwork={\n"
            f"\tssid={ssid_bytes.hex()}\n\tpsk={psk}\n\tkey_mgmt=WPA-PSK\n}}\n"
        )
        conf.chmod(0o600)
        # mcopy -m preserves this fixed FAT-compatible timestamp.
        os.utime(conf, (315532800, 315532800))
        image = directory / "boot.img"
        env = os.environ.copy()
        env.update(TZ="UTC", MTOOLS_SKIP_CHECK="1")
        try:
            subprocess.run(
                [mkfs, "-C", "-F", "12", "-r", "64", "-n", "P4_CONFIG", "--invariant", str(image), "256"],
                check=True, capture_output=True, env=env,
            )
            image.chmod(0o600)
            subprocess.run([mcopy, "-m", "-i", str(image), str(conf), "::/wpa_supplicant.conf"],
                           check=True, capture_output=True, env=env)
            if image.stat().st_size != 262144:
                raise ValueError("boot image has an unexpected size")
            # Link publishes without replacing an image created concurrently.
            os.link(image, args.output)
        except (OSError, ValueError, subprocess.CalledProcessError) as exc:
            parser.error(f"configuration image creation failed: {type(exc).__name__}")
    print(json.dumps({"bytes": args.output.stat().st_size,
                      "sha256": hashlib.sha256(args.output.read_bytes()).hexdigest(),
                      "operation": "created private configuration image; no device access"}))


if __name__ == "__main__":
    main()
