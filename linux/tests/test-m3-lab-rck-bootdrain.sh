#!/bin/sh
# rcK boot-drive drain (#42): the shutdown wait notices a pending or
# unobserved save, blocks until bootsyncd's marks clear, and gives up
# honestly on a stuck one.  Also checks bootsyncd still marks both the
# settle window and the import itself (the Stamp eject path).
set -u

repo_root=$(cd -- "$(dirname -- "$0")/../.." && pwd)
rck="$repo_root/linux/m3-lab/rootfs-overlay/etc/init.d/rcK"
daemon="$repo_root/linux/m3-lab/rootfs-overlay/usr/sbin/bootsyncd"
tmp=${TMPDIR:-/tmp}/rck-drain-test.$$
trap 'rm -rf -- "$tmp"' EXIT HUP INT TERM
mkdir -p "$tmp"

BOOT_IMG="$tmp/boot.img"
SEEN="$tmp/.bootsync-seen"
PENDING="$tmp/.bootsync-pending"
export BOOT_IMG BOOT_DRAIN_SEEN="$SEEN" BOOT_DRAIN_PENDING="$PENDING"

fails=0
ok()  { echo "ok: $1"; }
bad() { echo "FAIL: $1"; fails=$((fails + 1)); }

run_rck() { # -> $out, $rc
	out=$(sh "$rck" 2>&1)
	rc=$?
}

: > "$BOOT_IMG"; touch -r "$BOOT_IMG" "$SEEN"

# 1. Nothing pending: no mark, image not newer than SEEN_REF.
run_rck
[ "$rc" -eq 0 ] && [ -z "$out" ] && ok "quiet exit when nothing is pending" ||
	bad "idle rc=$rc out='$out'"

# 2. Pending mark cleared by the daemon mid-wait -> drains and exits 0.
: > "$PENDING"
( sleep 2; rm -f "$PENDING" ) &
run_rck; wait
[ "$rc" -eq 0 ] && echo "$out" | grep -q "save finished" &&
	ok "waits for the pending mark and reports the commit" ||
	bad "mark drain rc=$rc out='$out'"

# 3. An image swap the daemon has not polled yet is still caught: the
#    -nt check stands in until bootsyncd touches SEEN_REF.
touch "$BOOT_IMG"   # now strictly newer than SEEN_REF
( sleep 2; touch -r "$BOOT_IMG" "$SEEN" ) &
run_rck; wait
[ "$rc" -eq 0 ] && echo "$out" | grep -q "save finished" &&
	ok "an unobserved image swap blocks shutdown too" ||
	bad "nt drain rc=$rc out='$out'"

# 4. A stuck mark: bounded wait, honest message, nonzero exit.
: > "$PENDING"
BOOT_DRAIN_WAIT=3 run_rck
[ "$rc" -ne 0 ] && echo "$out" | grep -q "may not reach flash" &&
	ok "stuck save times out bounded and says so" ||
	bad "stuck rc=$rc out='$out'"
rm -f "$PENDING"

# 5. bootsyncd wiring: import_snapshot marks in-flight saves itself so a
#    USB eject import (no settle branch) is covered; the main loop drops
#    the mark right after the import call.
body=$(sed -n '/^import_snapshot()/,/^}/p' "$daemon")
echo "$body" | grep -q '^	pending_save_on' &&
	ok "import_snapshot marks the in-flight import" ||
	bad "import_snapshot lacks pending_save_on"
sed -n '/import_snapshot &&/,+1p' "$daemon" | grep -q 'pending_save_off' &&
	ok "main loop drops the mark after the import" ||
	bad "pending_save_off missing after import_snapshot"
grep -q 'pending_save_on' "$daemon" && grep -q 'PENDING_MARK="/run/.bootsync-pending"' "$daemon" &&
	ok "mark path unchanged: deepsleep's #56 wait still applies" ||
	bad "PENDING_MARK definition changed"

# 6. bootsyncd's own reboots (factory reset + the rc=2 staged image)
#    are not pending saves: each drops the mark and refreshes SEEN_REF
#    first, so rcK does not hold them for the full drain timeout.
n_rb=$(echo "$body" | grep -c '	reboot$')
n_off=$(echo "$body" | grep -B2 '	reboot$' | grep -c 'pending_save_off$')
n_ref=$(echo "$body" | grep -B1 '	reboot$' |
	grep -c 'touch -r "\$BOOT_IMG" "\$SEEN_REF"$')
[ "$n_rb" -eq 2 ] && [ "$n_off" -eq 2 ] && [ "$n_ref" -eq 2 ] &&
	ok "both self-reboots clear the mark and SEEN_REF first" ||
	bad "self-reboot drain: reboot=$n_rb off=$n_off ref=$n_ref"

# 7. inittab execs ::shutdown:rcK directly -- the file must be +x in
#    git and post-build.sh must chmod it (overlay rsync does not).
mode=$(git -C "$repo_root" ls-files -s \
	linux/m3-lab/rootfs-overlay/etc/init.d/rcK | awk '{print $1}')
[ "$mode" = "100755" ] &&
	ok "rcK is executable in git ($mode)" || bad "rcK git mode $mode"
grep -q 'chmod 0755.*etc/init.d/rcK' \
	"$repo_root/linux/m3-lab/post-build.sh" &&
	ok "post-build.sh chmods rcK" || bad "post-build.sh lacks rcK chmod"

[ "$fails" -eq 0 ] && echo "PASS: rcK boot-drive drain" || exit 1
