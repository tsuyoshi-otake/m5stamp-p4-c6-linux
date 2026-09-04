#!/bin/sh
set -eu

TARGET_DIR=${1:?Buildroot target directory is required}

# Keep the M1 rootfs small and deterministic. Buildroot owns the generated
# passwd/group database; no Wi-Fi credential or SSH key is introduced here.
install -d -m 0755 "${TARGET_DIR}/etc/init.d"
if [ -f "${TARGET_DIR}/etc/init.d/rcS" ]; then
	chmod 0755 "${TARGET_DIR}/etc/init.d/rcS"
fi
if [ -f "${TARGET_DIR}/etc/inittab" ]; then
	chmod 0644 "${TARGET_DIR}/etc/inittab"
fi
# Keep SDIO module ownership in S40network.  The stock mdev rule invokes
# modprobe for every modalias before init scripts can select the clock arm.
# Remove only that rule when the ESP-Hosted module is present; retain the
# standard device-permission rules for all other hotplugged devices.
sdio_module=
for candidate in \
	"${TARGET_DIR}"/lib/modules/*/updates/esp32_sdio.ko \
	"${TARGET_DIR}"/lib/modules/*/extra/esp32_sdio.ko; do
	if [ -f "$candidate" ]; then
		sdio_module=$candidate
		break
	fi
done
if [ -f "${TARGET_DIR}/etc/mdev.conf" ] && [ -n "$sdio_module" ]; then
	sed -i '/^\$MODALIAS=/d' "${TARGET_DIR}/etc/mdev.conf"
fi
# CONFIG_DEBUG_INFO is required in vmlinux for the diagnostic stacktrace, but
# Buildroot also leaves the DWARF sections in the out-of-tree ESP-Hosted
# module.  That module alone can exceed the 4 MiB effective physmap window
# used by the 7 MiB flash partition (physmap rounds the window down to a
# power-of-two size).  Strip only the target-rootfs copy; the unstripped
# module remains in the build output for offline analysis.
if [ "${EASYSTICK_STACKTRACE_DIAGNOSTICS:-0}" = "1" ]; then
	strip_tool=
	if [ -n "${TARGET_CROSS:-}" ] && [ -x "${TARGET_CROSS}strip" ]; then
		strip_tool="${TARGET_CROSS}strip"
	elif [ -n "${HOST_DIR:-}" ]; then
		for candidate in "${HOST_DIR}/bin/"*-buildroot-linux-uclibc-strip; do
			if [ -x "$candidate" ]; then
				strip_tool=$candidate
				break
			fi
		done
	fi
	[ -n "$strip_tool" ] || {
		echo "EASYSTICK_STACKTRACE: target cross-strip tool not found" >&2
		exit 1
	}
	[ -n "$sdio_module" ] || {
		echo "EASYSTICK_STACKTRACE: ESP-Hosted module missing" >&2
		exit 1
	}
	"$strip_tool" --strip-debug "$sdio_module"
	echo "EASYSTICK_STACKTRACE: stripped ESP-Hosted module debug sections"
fi
for init_script in \
	"${TARGET_DIR}/etc/init.d/S40network" \
	"${TARGET_DIR}/etc/init.d/S50dropbear"; do
	if [ -f "${init_script}" ]; then
		chmod 0755 "${init_script}"
	fi
done
if [ -f "${TARGET_DIR}/usr/sbin/easystick-sdio-diag" ]; then
	sed -i 's/\r$//' "${TARGET_DIR}/usr/sbin/easystick-sdio-diag"
	chmod 0755 "${TARGET_DIR}/usr/sbin/easystick-sdio-diag"
fi
