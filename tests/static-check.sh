#!/bin/sh
# Static checks for the rootfs scripts (#28).  Run from the repository root.
#   1. every script parses with sh -n
#   2. no script uses a command that the BusyBox build does not provide
#   3. no bare `<>` redirection (hush applies it to fd 1, not fd 0)
#   4. every overlay file that starts with #! is committed executable
#   5. shellcheck (if installed) reports no errors
set -u

root=$(cd "$(dirname "$0")/.." && pwd)
# The m3-lab image also carries the board overlays (sdio-diag).  Both the
# Stamp and Tab5 overlays are scanned; shadowed-file handling below uses
# the Stamp overlay path, which is the only one the release image ships.
board="$root/linux/buildroot-external/board/stamp-p4/rootfs-overlay"
boards="$board $root/linux/buildroot-external/board/tab5/rootfs-overlay"
scripts=$(find "$root/linux/m3-lab/rootfs-overlay" $boards \
	"$root/linux/m3-lab/post-build.sh" \
	-type f \( -path '*/init.d/*' -o -path '*/usr/sbin/*' -o -path '*/default.script.d/*' \
	-o -name post-build.sh \) 2>/dev/null)

# Commands known to be missing from the BusyBox build (see issue #7).
missing="pidof start-stop-daemon hwclock awk free top ip ifup ifdown wget telnet"

fail=0
for f in $scripts; do
	if ! sh -n "$f"; then
		echo "FAIL syntax: $f"
		fail=1
	fi
	# post-build.sh runs on the build host and only lists applet names.
	[ "${f##*/}" = post-build.sh ] && continue
	# Board overlay files that m3-lab shadows, or that post-build deletes,
	# never reach the image.  Both board overlays get the same treatment.
	for b in $boards; do
		case "$f" in
		"$b"/*)
			rel=${f#"$b"/}
			[ -e "$root/linux/m3-lab/rootfs-overlay/$rel" ] && continue 2
			grep -q "TARGET_DIR}/$rel\"" "$root/linux/m3-lab/post-build.sh" && continue 2
			;;
		esac
	done
	for cmd in $missing; do
		# Match the command in command position, ignoring comments.
		if grep -v '^[[:space:]]*#' "$f" | grep -Eq "(^|[^A-Za-z0-9_./-])$cmd([[:space:];|&)]|$)"; then
			echo "FAIL missing applet '$cmd' used in $f"
			fail=1
		fi
	done
	# The NOMMU /bin/sh is hush, whose bare `<>` opens on fd 1: `read <> fifo`
	# then reads the console and steals keystrokes.  Write `0<>`.
	if grep -v '^[[:space:]]*#' "$f" | grep -Eq '(^|[^0-9])<>'; then
		echo "FAIL bare '<>' (use '0<>') in $f"
		fail=1
	fi
done

# The image keeps the mode git records, and a 0644 script fails `[ -x ]`
# checks or exec (the udhcpc status hook never ran, #25).  Use the index:
# a Windows checkout does not show the executable bit.
if git -C "$root" rev-parse --git-dir >/dev/null 2>&1; then
	nonexec=$(git -C "$root" ls-files -s -- 'linux/*rootfs-overlay/*' |
		while read -r mode _ _ path; do
			[ "$mode" = 100644 ] || continue
			[ "$(head -c 2 "$root/$path")" = '#!' ] && echo "$path"
		done)
	for f in $nonexec; do
		echo "FAIL script not executable in git (git update-index --chmod=+x): $f"
		fail=1
	done
fi

if command -v shellcheck >/dev/null 2>&1; then
	# shellcheck for BusyBox ash: plain sh dialect, errors only.
	for f in $scripts; do
		shellcheck -s sh -S error "$f" || fail=1
	done
else
	echo "note: shellcheck not installed; skipped"
fi

# tab5_layout.h is the single source for the PSRAM windows the shim
# must keep the framebuffer away from; every guarded region must appear
# as a reserved-memory reg (or the Linux load addresses) in the Tab5
# DTS so a layout change on one side cannot silently desync.
layout="$root/linux/boot-shim/main/tab5_layout.h"
dts="$root/linux/dts/esp32p4-tab5.dts"
if [ -f "$layout" ] && [ -f "$dts" ]; then
	grep -oE '\{ 0x[0-9a-fA-F]+u, 0x[0-9a-fA-F]+u \}' "$layout" | \
	while read -r _ a s _; do
		addr=${a%u,}; size=${s%u}
		grep -q "reg = <$addr $size>" "$dts" && continue
		# The kernel/DTB load windows are not reserved-memory nodes;
		# they live at the memory-node base instead.
		[ "$addr" = "0x48000000" ] || [ "$addr" = "0x48800000" ] && continue
		echo "FAIL tab5_layout.h region $addr+$size missing from Tab5 DTS reserved-memory"
		exit 1
	done || fail=1
fi

[ "$fail" -eq 0 ] && echo "static checks passed"
exit "$fail"
