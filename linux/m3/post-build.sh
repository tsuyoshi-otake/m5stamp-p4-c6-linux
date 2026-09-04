#!/bin/sh
set -eu

TARGET_DIR=${1:?Buildroot target directory is required}

# Git checkout mode bits are not reliable on the Windows development host.
# Set the runtime modes explicitly instead of relying on overlay metadata.
for path in \
	"${TARGET_DIR}/etc/init.d/S10easystick-config" \
	"${TARGET_DIR}/etc/init.d/S85easystick-ssh" \
	"${TARGET_DIR}/usr/sbin/easystick-firstboot"; do
	if [ ! -f "${path}" ]; then
		echo "M3 file missing from rootfs: ${path}" >&2
		exit 1
	fi
	chmod 0755 "${path}"
done
chmod 0644 "${TARGET_DIR}/etc/inetd.conf"
