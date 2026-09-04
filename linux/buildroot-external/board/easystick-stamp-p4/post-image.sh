#!/bin/sh
set -eu

BINARIES_DIR=${BINARIES_DIR:?Buildroot did not provide BINARIES_DIR}

# Keep generated output in the caller-selected Buildroot output directory.
# The final image verifier is invoked separately so this hook never flashes
# a device or silently accepts a missing board artifact.
for artifact in Image rootfs.squashfs; do
	if [ ! -f "${BINARIES_DIR}/${artifact}" ]; then
		echo "M1 artifact not produced: ${BINARIES_DIR}/${artifact}" >&2
		exit 1
	fi
done
printf '%s\n' "M1 Buildroot artifacts present in ${BINARIES_DIR}"
