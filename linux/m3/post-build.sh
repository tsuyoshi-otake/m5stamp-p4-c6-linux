#!/bin/sh
set -eu

TARGET_DIR=${1:?Buildroot target directory is required}

# Git checkout mode bits are not reliable on the Windows development host.
# Set the runtime modes explicitly instead of relying on overlay metadata.
for path in \
	"${TARGET_DIR}/etc/init.d/S10config" \
	"${TARGET_DIR}/etc/init.d/S85ssh" \
	"${TARGET_DIR}/usr/sbin/firstboot"; do
	if [ ! -f "${path}" ]; then
		echo "M3 file missing from rootfs: ${path}" >&2
		exit 1
	fi
	chmod 0755 "${path}"
done
chmod 0644 "${TARGET_DIR}/etc/inetd.conf"

# NOMMU has no stack guard pages: the elf2flt default of 4 KB silently corrupts
# .data/.bss on overflow.  dropbear/dbclient need 256 KB: the ML-KEM-768 hybrid
# KEX uses >32 KB of stack and the overflow corrupted the uClibc sighandler[]
# table -> SIGILL on signal delivery (#54).
FLTHDR=""
for c in "${HOST_DIR:-/nonexistent}"/bin/*-flthdr; do
	[ -x "$c" ] && FLTHDR=$c && break
done
if [ -x "${FLTHDR}" ]; then
	for bin in usr/sbin/dropbear usr/bin/dbclient; do
		if [ -f "${TARGET_DIR}/${bin}" ]; then
			"${FLTHDR}" -s 262144 "${TARGET_DIR}/${bin}"
			"${FLTHDR}" "${TARGET_DIR}/${bin}" | grep -q "Stack Size:[[:space:]]*0x40000" || {
				echo "M3: bFLT stack check failed for ${bin}" >&2
				exit 1
			}
		fi
	done
else
	echo "M3: flthdr not found under HOST_DIR; dropbear stack left at 4 KB" >&2
fi
