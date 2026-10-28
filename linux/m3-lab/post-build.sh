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
rm -f "${TARGET_DIR}/etc/init.d/S10config"

# uClibc's utils install getconf as an ELF executable.  This NOMMU kernel runs
# only bFLT, so the shell fell back to BusyBox and printed "getconf: applet
# not found".
rm -f "${TARGET_DIR}/usr/bin/getconf"

for path in \
	"${TARGET_DIR}/etc/init.d/S05tmpfs" \
	"${TARGET_DIR}/etc/init.d/S40network" \
	"${TARGET_DIR}/etc/init.d/S45bootsync" \
	"${TARGET_DIR}/etc/init.d/S85ssh" \
	"${TARGET_DIR}/etc/init.d/S90watchdog" \
	"${TARGET_DIR}/usr/sbin/bootsyncd" \
	"${TARGET_DIR}/usr/sbin/usb-lun" \
	"${TARGET_DIR}/usr/sbin/watchdog-feed" \
	"${TARGET_DIR}/usr/sbin/update-status" \
	"${TARGET_DIR}/usr/sbin/autorun" \
	"${TARGET_DIR}/usr/sbin/deepsleep" \
	"${TARGET_DIR}/usr/lib/micropython/deepsleep.py" \
	"${TARGET_DIR}/usr/lib/micropython/lpcore.py" \
	"${TARGET_DIR}/usr/lib/board/autorun.sh" \
	"${TARGET_DIR}/etc/inetd.conf"; do
	if [ ! -f "${path}" ]; then
		echo "M3-lab file missing from rootfs: ${path}" >&2
		exit 1
	fi
done

chmod 0755 "${TARGET_DIR}/etc/init.d/S05tmpfs"
chmod 0755 "${TARGET_DIR}/etc/init.d/S40network"
chmod 0755 "${TARGET_DIR}/etc/init.d/S45bootsync"
chmod 0755 "${TARGET_DIR}/etc/init.d/S85ssh"
chmod 0755 "${TARGET_DIR}/etc/init.d/S90watchdog"
chmod 0755 "${TARGET_DIR}/usr/sbin/bootsyncd"
chmod 0755 "${TARGET_DIR}/usr/sbin/usb-lun"
chmod 0755 "${TARGET_DIR}/usr/sbin/watchdog-feed"
chmod 0755 "${TARGET_DIR}/usr/sbin/update-status"
chmod 0755 "${TARGET_DIR}/usr/sbin/sysinfo"
chmod 0755 "${TARGET_DIR}/usr/sbin/autorun"
chmod 0755 "${TARGET_DIR}/usr/sbin/deepsleep"
chmod 0644 "${TARGET_DIR}/usr/lib/micropython/deepsleep.py"
chmod 0644 "${TARGET_DIR}/usr/lib/micropython/lpcore.py"
# Overrides Buildroot's udhcpc script: the m3-lab copy sources its *.d
# hooks instead of exec'ing them (#40).
chmod 0755 "${TARGET_DIR}/usr/share/udhcpc/default.script"
chmod 0644 "${TARGET_DIR}/etc/inetd.conf"

# BusyBox applet reduction (busybox.fragment) removed start-stop-daemon and
# hwclock; ntpd is only used one-shot by S40network (#25), not as a daemon.
# Init scripts that depend on them can only print "applet not found" and
# FAIL, and S01seedrng runs before the /var/lib overlay exists
# (read-only rootfs).  Drop them from the lab image.
rm -f "${TARGET_DIR}/etc/init.d/S01syslogd" \
	"${TARGET_DIR}/etc/init.d/S01seedrng" \
	"${TARGET_DIR}/etc/init.d/S41timesync"

# Remove applet symlinks whose applet is not built into this BusyBox, so users
# get "not found" from the shell instead of "applet not found".
for applet in \
	base32 dnsdomainname link nuke resume setpriv setserial stty zcat \
	freeramdisk hwclock ifdown ifup ip ipaddr iplink iproute mkdosfs \
	start-stop-daemon ascii bc bzcat cksum crc32 dos2unix factor fallocate \
	fold getfattr hexedit lsscsi lzcat lzopcat mesg nl nproc od paste \
	setfattr sha1sum sha3sum sha512sum shred svc svok telnet tree truncate \
	ts tsort unix2dos unlink unlzop uudecode uuencode w wget who whoami \
	xzcat arping fsfreeze i2ctransfer mim partprobe; do
	for dir in bin sbin usr/bin usr/sbin; do
		link="${TARGET_DIR}/${dir}/${applet}"
		if [ -L "${link}" ]; then
			case "$(readlink "${link}")" in
				*busybox) rm -f "${link}" ;;
			esac
		fi
	done
done
# The list above is hand-kept; busybox.links is what this build compiled in
# (#28).  Drop any other BusyBox symlink left over from an earlier config.
bb_links=$(ls "${BUILD_DIR:-/nonexistent}"/busybox-*/busybox.links 2>/dev/null | head -n 1)
if [ -n "${bb_links}" ]; then
	find "${TARGET_DIR}/bin" "${TARGET_DIR}/sbin" "${TARGET_DIR}/usr/bin" \
		"${TARGET_DIR}/usr/sbin" -type l 2>/dev/null | while read -r link; do
		case "$(readlink "${link}")" in
			*busybox) ;;
			*) continue ;;
		esac
		grep -Fqx "/${link#"${TARGET_DIR}"/}" "${bb_links}" || {
			echo "M3-lab: removing stale applet link ${link#"${TARGET_DIR}"/}"
			rm -f "${link}"
		}
	done
fi

# Resident daemons run from the minimal busybox-daemons (#23): on NOMMU each
# process holds a private copy of its binary's text, about 370 KB for the
# full BusyBox.  The full image keeps the init applet (Buildroot forces it),
# so repoint /sbin/init; linuxrc is unused (no initramfs).
if [ -x "${TARGET_DIR}/bin/busybox-daemons" ]; then
	rm -f "${TARGET_DIR}/linuxrc"
	for link in sbin/init usr/sbin/inetd sbin/syslogd usr/sbin/udhcpd; do
		case "${link}" in
			usr/*) target=../../bin/busybox-daemons ;;
			*) target=../bin/busybox-daemons ;;
		esac
		ln -sfn "${target}" "${TARGET_DIR}/${link}"
	done
fi

# #45: udhcpc keeps a lease-lifetime process; it runs from its own minimal
# BusyBox instead of the full image (a dedicated binary, not
# busybox-daemons, so init/inetd/syslogd do not grow).
if [ -x "${TARGET_DIR}/bin/busybox-udhcpc" ]; then
	ln -sfn ../bin/busybox-udhcpc "${TARGET_DIR}/sbin/udhcpc"
fi

# LAB_TOOLS=0 (#32) builds without dbclient; drop the other
# bring-up entry points with it.
if [ -n "${BR2_CONFIG:-}" ] &&
   grep -qx '# BR2_PACKAGE_DROPBEAR_CLIENT is not set' "${BR2_CONFIG}"; then
	rm -f "${TARGET_DIR}/usr/bin/dropbearconvert" "${TARGET_DIR}/usr/bin/nc"
fi

# Lock root: an empty root password would give passwordless root as soon as a
# getty/login is used.  SSH takes root by key only (-g), and
# credentials.sh lifts the lock at runtime from root_password= (#37).
if [ -f "${TARGET_DIR}/etc/shadow" ]; then
	sed -i 's/^root:[^:]*:/root:!:/' "${TARGET_DIR}/etc/shadow"
fi

# NOMMU has no stack guard pages: the elf2flt default of 4 KB silently corrupts
# .data/.bss on overflow.  Raise the bFLT stack for the network-facing daemons.
# dropbear/dbclient need 256 KB: the ML-KEM-768 hybrid KEX (default since
# OpenSSH 9.x clients) uses >32 KB of stack and the overflow corrupted the
# uClibc sighandler[] table -> SIGILL on signal delivery (#54).  32 KB
# (same as micropython) keeps three wpa_supplicant/tcpdump children within
# the RAM budget.  boot-commit was left at 4 KB.
FLTHDR=""
for c in "${HOST_DIR:-/nonexistent}"/bin/*-flthdr; do
	[ -x "$c" ] && FLTHDR=$c && break
done
[ -n "${FLTHDR}" ] || { echo "M3-lab: flthdr not found under HOST_DIR" >&2; exit 1; }
if [ -x "${FLTHDR}" ]; then
	for bin in usr/sbin/dropbear usr/bin/dbclient; do
		if [ -f "${TARGET_DIR}/${bin}" ]; then
			"${FLTHDR}" -s 262144 "${TARGET_DIR}/${bin}"
			# Fail the build if the stack did not stick (#28, #54).
			"${FLTHDR}" "${TARGET_DIR}/${bin}" | grep -q "Stack Size:[[:space:]]*0x40000" || {
				echo "M3-lab: bFLT stack check failed for ${bin}" >&2
				exit 1
			}
		fi
	done
	for bin in usr/sbin/wpa_supplicant usr/sbin/iw \
		usr/bin/tcpdump usr/sbin/boot-commit; do
		if [ -f "${TARGET_DIR}/${bin}" ]; then
			"${FLTHDR}" -s 32768 "${TARGET_DIR}/${bin}"
			# Fail the build if the stack did not stick (#28).
			"${FLTHDR}" "${TARGET_DIR}/${bin}" | grep -q "Stack Size:[[:space:]]*0x8000" || {
				echo "M3-lab: bFLT stack check failed for ${bin}" >&2
				exit 1
			}
		fi
	done
	# Small helpers only need BusyBox's floor.
	for bin in usr/sbin/respawn usr/sbin/mdnsd usr/sbin/run-system; do
		if [ -f "${TARGET_DIR}/${bin}" ]; then
			"${FLTHDR}" -s 16384 "${TARGET_DIR}/${bin}"
		fi
	done
	# Every other bFLT must keep at least BusyBox's 16000 bytes (#18).
	find "${TARGET_DIR}" -type f | while read -r bin; do
		[ "$(head -c 4 "${bin}")" = bFLT ] || continue
		stack=$("${FLTHDR}" "${bin}" | sed -n 's/.*Stack Size:[[:space:]]*//p')
		if [ -z "${stack}" ] || [ "$((stack))" -lt 16000 ]; then
			echo "M3-lab: bFLT stack ${stack:-unknown} below 16000 in ${bin#"${TARGET_DIR}"/}" >&2
			exit 1
		fi
	done || exit 1
fi

# Boot FAT template (#8, #22): the USB drive image used on first boot and as
# the flashed boot.img.  Generate it here from text so a clean checkout (CI)
# has it and its hash is reproducible.  S05tmpfs fills in the rest
# (cmdline.txt reference, credentials.txt) on first boot.
make_boot_template() {
	out="${TARGET_DIR}/etc/board/boot-template.img"
	wpa="${TARGET_DIR}/etc/board/wpa_supplicant.conf"
	[ -f "${wpa}" ] || { echo "M3-lab: ${wpa} missing" >&2; exit 1; }
	mkfs=$(command -v mkfs.vfat || command -v mkfs.fat || echo /sbin/mkfs.vfat)
	stage=$(mktemp -d)
	cat > "${stage}/config.txt" <<'EOF'
# Stamp-P4 Boot & Hardware Configuration
# Device: M5Stamp ESP32-P4 + C6 SDIO (Stamp-P4)
# hostname is applied at boot by S40network.
hostname=stamp-p4
# wifi_mode: ap or sta.  Leave unset to auto-detect from mode=2 in wpa_supplicant.conf.
#wifi_mode=ap
# wifi_ssid/wifi_psk replace wpa_supplicant.conf (station, or SoftAP with wifi_mode=ap).
#wifi_ssid=MyRouter
#wifi_psk=MyPassphrase
# Optional management frame protection (802.11w) with wifi_psk.
#wifi_pmf=1
# Regulatory domain (two-letter country code); unset keeps the world domain.
#country=JP
# Keys only over SSH; needs an authorized_keys file on this drive.
#ssh_password_auth=no
# root SSH login is key-only: put a root_authorized_keys file on this drive
# (#37).  su uses root_password= in credentials.txt.
# SDIO clock to the C6 in MHz (1, 2, 5, 10, 20, 40), capped by the DTB.
#sdio_mhz=5
# Reset the board if userland stops responding or SSH (inetd) stays down.
#watchdog=1
# maxcpus is fixed in the DTB; this file does not change the CPU count.
# Put an empty file named factory-reset on this drive and reboot to restore
# these defaults and generate new credentials.
EOF
	cp "${wpa}" "${stage}/wpa_supplicant.conf"
	rm -f "${out}"
	"${mkfs}" -C -F 12 -r 64 -n STAMP_P4 --invariant "${out}" 256 >/dev/null
	# FAT cannot store dates before 1980.
	epoch=${SOURCE_DATE_EPOCH:-315532800}
	[ "${epoch}" -ge 315532800 ] || epoch=315532800
	for f in "${stage}"/*; do
		touch -d "@${epoch}" "${f}"
		MTOOLS_SKIP_CHECK=1 mcopy -m -i "${out}" "${f}" ::/
	done
	rm -rf "${stage}"
	chmod 0644 "${out}"
	if [ -n "${BINARIES_DIR:-}" ]; then
		mkdir -p "${BINARIES_DIR}"
		cp "${out}" "${BINARIES_DIR}/boot.img"
	fi
}
make_boot_template

# Build identification (#22): which commit produced this rootfs.
# build-m1.sh exports these from the real checkout; the staged copy is not a git tree.
GIT_REV=${ES_GIT_REV:-unknown}
GIT_DIRTY=${ES_GIT_DIRTY:-0}
mkdir -p "${TARGET_DIR}/etc"
# S05 mounts the FAT boot image here; the squashfs is read-only at runtime,
# so the mount point must already exist in the image.
mkdir -p "${TARGET_DIR}/boot"
{
	echo "version=${ES_VERSION:-unknown}"
	echo "git=${GIT_REV}"
	echo "dirty=${GIT_DIRTY}"
	echo "build_date=$(date -u -d "@${SOURCE_DATE_EPOCH:-$(date +%s)}" +%Y-%m-%dT%H:%M:%SZ)"
} > "${TARGET_DIR}/etc/build-info"

# No dangling symlinks (#28), except those into runtime filesystems.
find "${TARGET_DIR}" -type l | while read -r link; do
	t=$(readlink "${link}")
	case "${t}" in
		/*) p="${TARGET_DIR}${t}" ;;
		*) p="$(dirname "${link}")/${t}" ;;
	esac
	[ -e "${p}" ] && continue
	case "${t}" in
		*proc/*|*tmp/*|*/run/*|/run/*|*dev/*|*sys/*) continue ;;
	esac
	echo "M3-lab: dangling symlink ${link#"${TARGET_DIR}"/} -> ${t}" >&2
	exit 1
done || exit 1
