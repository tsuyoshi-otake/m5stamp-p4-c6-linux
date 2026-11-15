#!/bin/sh
# boot-edit (tab5 #20): the on-board replacement for the USB drive.  The
# FAT mount is stubbed with a deterministic tar round trip, so the script's
# own logic runs on any host: argument checks, the read-only modes, the
# no-change shortcut, the atomic swap, the wait for bootsyncd, the lock and
# the refusal on a board that has a USB drive.
set -u

repo_root=$(cd -- "$(dirname -- "$0")/../.." && pwd)
tool="$repo_root/linux/m3-lab/rootfs-overlay/usr/sbin/boot-edit"
tmp=${TMPDIR:-/tmp}/boot-edit-test.$$
saver=""
trap '[ -n "$saver" ] && kill "$saver" 2>/dev/null; rm -rf -- "$tmp"' EXIT HUP INT TERM
mkdir -p "$tmp/bin" "$tmp/run" "$tmp/sys" "$tmp/drive" "$tmp/seed"

pack() { # dir image
	tar --sort=name --mtime=@0 --owner=0 --group=0 --numeric-owner --mode=u=rwX,go= \
		-cf "$2" -C "$1" .
}

# mount -t vfat -o OPTS IMAGE DIR: unpack; remember where a writable mount
# came from so umount can pack it back.
cat > "$tmp/bin/mount" <<EOF
#!/bin/sh
opts=\$4 img=\$5 dir=\$6
tar -xf "\$img" -C "\$dir" || exit 1
case "\$opts" in *rw*) echo "\$img" > "$tmp/rw-mount" ;; esac
exit 0
EOF
cat > "$tmp/bin/umount" <<EOF
#!/bin/sh
if [ -f "$tmp/rw-mount" ]; then
	read -r img < "$tmp/rw-mount"
	rm -f "$tmp/rw-mount"
	tar --sort=name --mtime=@0 --owner=0 --group=0 --numeric-owner --mode=u=rwX,go= \
		-cf "\$img" -C "\$1" . || exit 1
fi
rm -rf "\$1"/* "\$1"/.[!.]* 2>/dev/null
exit 0
EOF
# An editor that appends one line, standing in for vi.
cat > "$tmp/bin/append-editor" <<'EOF'
#!/bin/sh
echo "edited=yes" >> "$1"
EOF
chmod +x "$tmp/bin/mount" "$tmp/bin/umount" "$tmp/bin/append-editor"

printf 'hostname=tab5\n' > "$tmp/seed/config.txt"
img="$tmp/drive/boot.img"
pack "$tmp/seed" "$img"

PATH="$tmp/bin:$PATH"
BOOT_IMG="$img"
BOOT_EDIT_RUN_DIR="$tmp/run"
RUN_SYSTEM_CLASS_UDC_ROOT="$tmp/sys"
BOOT_EDIT_SAVE_WAIT=1
export PATH BOOT_IMG BOOT_EDIT_RUN_DIR RUN_SYSTEM_CLASS_UDC_ROOT BOOT_EDIT_SAVE_WAIT

fails=0
ok() { echo "ok: $1"; }
bad() { echo "FAIL: $1"; fails=$((fails + 1)); }
sum() { md5sum "$1" | cut -d' ' -f1; }
clean() { # no lock and no working copy may outlive a run
	[ ! -e "$tmp/run/.boot-edit.lock" ] || bad "$1: lock left behind"
	for d in "$tmp/drive"/.boot-edit.*; do
		[ -e "$d" ] && bad "$1: working copy left behind"
	done
}
# What bootsyncd does after a change: record the hash it saved to flash.
start_saver() {
	( while :; do sum "$img" > "$tmp/run/.boot-flash-md5.new" &&
		mv -f "$tmp/run/.boot-flash-md5.new" "$tmp/run/.boot-flash-md5"
		sleep 0.2; done ) &
	saver=$!
}
stop_saver() {
	kill "$saver" 2>/dev/null; wait "$saver" 2>/dev/null; saver=""
	rm -f "$tmp/run/.boot-flash-md5"
}

before=$(sum "$img")

# 1. Read-only modes print and change nothing.
out=$(sh "$tool" --ls 2>&1) && echo "$out" | grep -q "config.txt" &&
	ok "--ls lists the drive" || bad "--ls: $out"
out=$(sh "$tool" --cat config.txt 2>&1) && [ "$out" = "hostname=tab5" ] &&
	ok "--cat prints the file" || bad "--cat: $out"
[ "$(sum "$img")" = "$before" ] && ok "reads leave the image alone" ||
	bad "a read changed the image"
clean "reads"

# 2. Writing the same bytes is not a change.
out=$(printf 'hostname=tab5\n' | sh "$tool" --put config.txt 2>&1) &&
	echo "$out" | grep -q "no change" && [ "$(sum "$img")" = "$before" ] &&
	ok "identical content is reported as no change" || bad "no-change: $out"
clean "no-change"

# 3. A real change swaps the image in and waits for the save.
start_saver
out=$(printf 'hostname=desk\n' | sh "$tool" --put config.txt 2>&1); rc=$?
stop_saver
[ "$rc" -eq 0 ] && echo "$out" | grep -q "saved to flash" &&
	ok "--put reports the save" || bad "--put rc=$rc: $out"
[ "$(sum "$img")" != "$before" ] &&
	[ "$(sh "$tool" --cat config.txt 2>&1)" = "hostname=desk" ] &&
	ok "--put content is on the drive" || bad "--put content missing"
clean "put"

# 4. Without a save the command says so and fails, but the drive keeps the
#    change (bootsyncd may still be busy, or may have rejected the image).
out=$(printf 'k1\n' | sh "$tool" --put authorized_keys 2>&1); rc=$?
[ "$rc" -eq 1 ] && echo "$out" | grep -q "not confirmed" &&
	[ "$(sh "$tool" --cat authorized_keys 2>&1)" = "k1" ] &&
	ok "an unconfirmed save is reported and the change stays" ||
	bad "unconfirmed rc=$rc: $out"
clean "unconfirmed"

# 5. The editor path and delete.
start_saver
out=$(EDITOR=append-editor sh "$tool" config.txt 2>&1); rc=$?
[ "$rc" -eq 0 ] && sh "$tool" --cat config.txt 2>&1 | grep -q "edited=yes" &&
	ok "editing a file changes it" || bad "edit rc=$rc: $out"
out=$(sh "$tool" --rm authorized_keys 2>&1); rc=$?
stop_saver
[ "$rc" -eq 0 ] && ! sh "$tool" --ls 2>&1 | grep -q "authorized_keys" &&
	ok "--rm deletes the file" || bad "--rm rc=$rc: $out"
clean "edit/rm"

# 6. Bad arguments are refused before anything is touched.
before=$(sum "$img")
for args in "" "--put" "--ls extra" "--bogus x" "a b"; do
	# shellcheck disable=SC2086
	sh "$tool" $args </dev/null >/dev/null 2>&1; rc=$?
	[ "$rc" -eq 2 ] || bad "usage: '$args' rc=$rc"
done
for name in "../config.txt" "dir/file" ".hidden"; do
	sh "$tool" --put "$name" </dev/null >/dev/null 2>&1; rc=$?
	[ "$rc" -eq 1 ] || bad "path name '$name' rc=$rc"
done
[ "$(sum "$img")" = "$before" ] && ok "bad arguments change nothing" ||
	bad "bad arguments changed the image"
clean "arguments"

# 7. A live lock holder blocks; a dead one does not.
mkdir "$tmp/run/.boot-edit.lock"
echo "$$" > "$tmp/run/.boot-edit.lock/pid"
out=$(sh "$tool" --ls 2>&1); rc=$?
[ "$rc" -eq 1 ] && echo "$out" | grep -q "another boot-edit" &&
	ok "a running boot-edit blocks a second one" || bad "lock rc=$rc: $out"
( exit 0 ) & dead=$!; wait "$dead"
echo "$dead" > "$tmp/run/.boot-edit.lock/pid"
out=$(sh "$tool" --ls 2>&1) && ok "a stale lock is taken over" ||
	bad "stale lock: $out"
clean "lock"

# 8. A board with a USB device controller is refused: the PC owns the drive.
mkdir "$tmp/sys/udc0"
out=$(printf 'x\n' | sh "$tool" --put config.txt 2>&1); rc=$?
rmdir "$tmp/sys/udc0"
[ "$rc" -eq 1 ] && echo "$out" | grep -q "USB drive" &&
	[ "$(sum "$img")" = "$before" ] &&
	ok "refused where a USB drive exists" || bad "udc rc=$rc: $out"
clean "udc"

[ "$fails" -eq 0 ] && echo "PASS: boot-edit drive changes" || exit 1
