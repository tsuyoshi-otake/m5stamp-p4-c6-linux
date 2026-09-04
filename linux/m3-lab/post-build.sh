#!/bin/sh
set -eu

TARGET_DIR=${1:?Buildroot target directory is required}

# The generic module package used to install an unconditional modules-load.d
# entry.  Loading without board parameters locks resetpin=-1/clockspeed=0 for
# the lifetime of this NOMMU kernel, where module unload is unavailable.
rm -f "${TARGET_DIR}/etc/modules-load.d/esp-hosted-ng.conf"

# The production M3 profile performs a writable-/config gate before starting
# SSH.  The lab image deliberately uses a volatile Dropbear key and the
# Raspberry-Pi-style bring-up account, so that gate must not run first on its
# read-only squashfs root.  Keep the production script in m3/ unchanged.
rm -f "${TARGET_DIR}/etc/init.d/S10easystick-config"

for path in \
	"${TARGET_DIR}/etc/init.d/S05easystick-tmpfs" \
	"${TARGET_DIR}/etc/init.d/S40network" \
	"${TARGET_DIR}/etc/init.d/S85easystick-ssh" \
	"${TARGET_DIR}/etc/inetd.conf"; do
	if [ ! -f "${path}" ]; then
		echo "M3-lab file missing from rootfs: ${path}" >&2
		exit 1
	fi
done

chmod 0755 "${TARGET_DIR}/etc/init.d/S05easystick-tmpfs"
chmod 0755 "${TARGET_DIR}/etc/init.d/S40network"
chmod 0755 "${TARGET_DIR}/etc/init.d/S85easystick-ssh"
chmod 0644 "${TARGET_DIR}/etc/inetd.conf"
