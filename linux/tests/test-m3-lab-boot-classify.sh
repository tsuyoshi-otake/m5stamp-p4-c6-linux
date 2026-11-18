#!/bin/sh
# boot-classify + the #43 no-save gate: valid image, all-zero, random,
# truncated, unreadable, valid-signature-but-mount-fails, and the
# bootsyncd marker check that keeps a template-derived image off flash.
set -u

repo_root=$(cd -- "$(dirname -- "$0")/../.." && pwd)
helper="$repo_root/linux/m3-lab/rootfs-overlay/usr/sbin/boot-classify"
daemon="$repo_root/linux/m3-lab/rootfs-overlay/usr/sbin/bootsyncd"
tmp=${TMPDIR:-/tmp}/boot-classify-test.$$
trap 'chmod -Rf u+rw "$tmp" 2>/dev/null; rm -rf -- "$tmp"' EXIT HUP INT TERM
mkdir -p "$tmp/bin" "$tmp/run" "$tmp/mnt"

# Stub mount/umount: MOUNT_OK controls the answer.
cat > "$tmp/bin/mount" <<'EOF'
#!/bin/sh
[ "${MOUNT_OK:-1}" = 1 ] && exit 0
exit 1
EOF
cat > "$tmp/bin/umount" <<'EOF'
#!/bin/sh
exit 0
EOF
chmod +x "$tmp/bin/mount" "$tmp/bin/umount"
PATH="$tmp/bin:$PATH"
export PATH

BOOT_CLASSIFY_CMDLINE="$tmp/cmdline"
: > "$BOOT_CLASSIFY_CMDLINE"
BOOT_CLASSIFY_MNT="$tmp/mnt"
BOOT_CLASSIFY_SYSFS="$tmp/bootfat"
export BOOT_CLASSIFY_CMDLINE BOOT_CLASSIFY_MNT BOOT_CLASSIFY_SYSFS MOUNT_OK

fails=0
check() { # name expected actual
	if [ "$2" = "$3" ]; then
		echo "ok: $1"
	else
		echo "FAIL: $1: expected '$2' got '$3'"
		fails=$((fails + 1))
	fi
}

mkimg() { # path -- 262144 zero bytes with the FAT signature at 11 and 510
	dd if=/dev/zero of="$1" bs=4096 count=64 2>/dev/null
	printf '\x00\x02' | dd of="$1" bs=1 seek=11 conv=notrunc 2>/dev/null
	printf '\x55\xaa' | dd of="$1" bs=1 seek=510 conv=notrunc 2>/dev/null
}

# 1. Valid signed image, mount stub succeeds.
img="$tmp/valid.img"
mkimg "$img"
MOUNT_OK=1
check "valid image" ok "$("$helper" "$img")"

# 2. All-zero 262144 bytes: right size, no signature.  The shim says
# "blank" (no CRC-valid A/B record), so regenerating is safe.
img="$tmp/zero.img"
dd if=/dev/zero of="$img" bs=4096 count=64 2>/dev/null
echo blank > "$BOOT_CLASSIFY_SYSFS"
check "all-zero image, shim blank" no-signature "$("$helper" "$img")"

# 3. Random bytes: no signature either, same blank verdict.
img="$tmp/rand.img"
dd if=/dev/urandom of="$img" bs=4096 count=64 2>/dev/null
check "random image, shim blank" no-signature "$("$helper" "$img")"

# 4. Truncated file.
img="$tmp/trunc.img"
dd if=/dev/zero of="$img" bs=4096 count=10 2>/dev/null
check "truncated image" read-fail "$("$helper" "$img")"

# 5. Unreadable file: a directory reads as an error everywhere
# (chmod 000 does not block reads in some test environments).
img="$tmp/unread.img"
mkdir -p "$img"
check "unreadable image" read-fail "$("$helper" "$img")"

# 6. Right size + signature, mount fails (kernel cannot mount vfat).
img="$tmp/nomnt.img"
mkimg "$img"
MOUNT_OK=0
check "mount-fails image" mount-fail "$("$helper" "$img")"
MOUNT_OK=1

# 7. Missing file entirely.
check "missing image" read-fail "$("$helper" "$tmp/gone.img")"

# 8. Forced-state cmdline word (board test hook): read-fail and
# mount-fail only.  Removed words must NOT force a state that permits
# a flash save.
img="$tmp/valid2.img"
mkimg "$img"
echo "console=ttyGS1 es_bootfat=mount-fail root=x" > "$BOOT_CLASSIFY_CMDLINE"
check "forced mount-fail" mount-fail "$("$helper" "$img")"
echo "es_bootfat=read-fail" > "$BOOT_CLASSIFY_CMDLINE"
check "forced read-fail" read-fail "$("$helper" "$img")"
echo "es_bootfat=no-signature" > "$BOOT_CLASSIFY_CMDLINE"
check "removed no-signature hook" ok "$("$helper" "$img")"
echo "es_bootfat=ok" > "$BOOT_CLASSIFY_CMDLINE"
check "removed ok hook" ok "$("$helper" "$img")"
: > "$BOOT_CLASSIFY_CMDLINE"

# 8b. No-signature evidence gate (#43 review): with a signature-less
# image, the shim verdict decides.
img="$tmp/nosig.img"
dd if=/dev/zero of="$img" bs=4096 count=64 2>/dev/null
echo valid > "$BOOT_CLASSIFY_SYSFS"
check "no-sig + shim valid" read-fail "$("$helper" "$img")"
echo blank > "$BOOT_CLASSIFY_SYSFS"
check "no-sig + shim blank" no-signature "$("$helper" "$img")"
echo unknown > "$BOOT_CLASSIFY_SYSFS"
check "no-sig + shim unknown" no-signature "$("$helper" "$img")"
rm -f "$BOOT_CLASSIFY_SYSFS"
check "no-sig + no shim node" no-signature "$("$helper" "$img")"

# 9. bootsyncd must not write while the marker exists: extract
# import_snapshot, stub its deps, set the marker, assert it returns
# before touching the image or boot-commit.
sed -n '/^import_snapshot()/,/^}/p' "$daemon" > "$tmp/funcs.sh"
grep -q 'NOSAVE_MARK' "$tmp/funcs.sh" || {
	echo "FAIL: no-save gate missing from import_snapshot"
	exit 1
}
cat > "$tmp/stub.sh" <<EOF
log() { echo "[bootsync] \$1" >> "$tmp/log"; }
release() { :; }
NOSAVE_MARK="$tmp/run/.boot-nosave"
BOOT_IMG="$tmp/drive.img"
IMPORT_MODE=settle
SEEN_REF="$tmp/ref"
MNT_POINT="$tmp/mnt_snap"
BOOT_COMMIT="$tmp/bin/boot-commit"
EOF
cat > "$tmp/bin/boot-commit" <<EOF
#!/bin/sh
echo commit >> "$tmp/commit-calls"
exit 0
EOF
chmod +x "$tmp/bin/boot-commit"
echo "test reason: kernel mount-fail" > "$tmp/run/.boot-nosave"
dd if=/dev/zero of="$tmp/drive.img" bs=4096 count=64 2>/dev/null
: > "$tmp/ref"
out=$( . "$tmp/stub.sh"; . "$tmp/funcs.sh"; import_snapshot; echo rc=$? )
case "$out" in *rc=0*) echo "ok: marker import returns 0" ;;
	*) echo "FAIL: import_snapshot rc: $out"; fails=$((fails + 1)) ;;
esac
if [ -f "$tmp/commit-calls" ]; then
	echo "FAIL: boot-commit ran while the marker existed"
	fails=$((fails + 1))
else
	echo "ok: no flash write with marker"
fi
grep -q "NOT saved: test reason" "$tmp/log" 2>/dev/null &&
	echo "ok: NOT-saved log line" ||
	{ echo "FAIL: missing NOT-saved log"; fails=$((fails + 1)); }
rm -f "$tmp/run/.boot-nosave"

# 10. The credentials fast-path in S05tmpfs calls boot-commit directly
# and must honour the same marker — a real board leaked template saves
# through it before the gate existed.
s05="$repo_root/linux/m3-lab/rootfs-overlay/etc/init.d/S05tmpfs"
if awk '/CRED_NEW:-0/,/boot-commit/' "$s05" | grep -q 'boot-nosave'; then
	echo "ok: credentials boot-commit gated by marker"
else
	echo "FAIL: credentials boot-commit not gated"
	fails=$((fails + 1))
fi

if [ "$fails" -gt 0 ]; then
	echo "FAILED: $fails checks"
	exit 1
fi
echo "PASS"
