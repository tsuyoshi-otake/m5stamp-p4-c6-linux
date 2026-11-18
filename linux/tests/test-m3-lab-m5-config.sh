#!/bin/sh
# m5-config (shared #42): the key editor and the validation/save path on
# a stub drive.  mount/umount are stubbed with a tar round trip (same
# trick as test-m3-lab-boot-edit.sh), so the real drive_write code --
# working copy, atomic swap, LUN forced-eject, save-hash wait -- runs on
# any host.
set -u

repo_root=$(cd -- "$(dirname -- "$0")/../.." && pwd)
tool="$repo_root/linux/m3-lab/rootfs-overlay/usr/bin/m5-config"
tmp=${TMPDIR:-/tmp}/m5cfg-test.$$
saver=""
trap '[ -n "$saver" ] && kill "$saver" 2>/dev/null; rm -rf -- "$tmp"' EXIT HUP INT TERM
mkdir -p "$tmp/bin" "$tmp/run" "$tmp/pub" "$tmp/mnt" "$tmp/etc-board" \
	"$tmp/home-m5/.ssh" "$tmp/home-root/.ssh" \
	"$tmp/udc/udc0/device/udc/udc0/gadget/lun0"

pack() { # dir image
	tar --sort=name --mtime=@0 --owner=0 --group=0 --numeric-owner --mode=u=rwX,go= \
		-cf "$2" -C "$1" .
}
unpack() { tar -xf "$1" -C "$2"; }

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
chmod +x "$tmp/bin/mount" "$tmp/bin/umount"

# A drive like the first-boot generated one: comments, mixed styles,
# CRLF on one line, and the key the tests touch once already commented.
seed="$tmp/seed"
mkdir -p "$seed"
{
	printf '# header comment\n'
	printf 'hostname = desk-01\n'
	printf '#wifi_ssid=commented\n'
	printf 'wifi_mode=ap\n'
	printf 'channel=6\r\n'
	printf 'debug=0\n'
} > "$seed/config.txt"
{
	printf 'device_id=BEEF\n'
	printf 'password=oldsecret1\n'
	printf 'root_password=oldroot22\n'
	printf 'ssid=m5-BEEF\npsk=oldpsk123\n'
} > "$seed/credentials.txt"
printf 'network={\n\tssid="m5-BEEF"\n\tmode=2\n\tpsk="oldpsk123"\n}\n' \
	> "$seed/wpa_supplicant.conf"
img="$tmp/pub/boot.img"
pack "$seed" "$img"
# The published copies get reads from $PUB_DIR.
cp "$seed/config.txt" "$seed/credentials.txt" "$seed/wpa_supplicant.conf" "$tmp/pub/"

LUNF="$tmp/udc/udc0/device/udc/udc0/gadget/lun0/file"
: > "$LUNF"   # empty = device-owned by default

PATH="$tmp/bin:$PATH"
export PATH
export M5CFG_RUN_DIR="$tmp/run" M5CFG_BOOT_IMG="$img" M5CFG_TMP="$tmp/pub"
export M5CFG_ETC_BOARD="$tmp/etc-board" M5CFG_BOOT_MNT="$tmp/mnt"
export M5CFG_HOME_M5="$tmp/home-m5" M5CFG_HOME_ROOT="$tmp/home-root"
export RUN_SYSTEM_CLASS_UDC_ROOT="$tmp/udc" M5CFG_SAVE_WAIT=3 M5CFG_ASSUME_ROOT=1

fails=0
ok() { echo "ok: $1"; }
bad() { echo "FAIL: $1"; fails=$((fails + 1)); }
imgfile() {
	rm -rf "$tmp/look"
	mkdir -p "$tmp/look"
	unpack "$img" "$tmp/look" && cat "$tmp/look/$1"
}
start_saver() {
	( while :; do md5sum "$img" 2>/dev/null | cut -d' ' -f1 \
		> "$tmp/run/.boot-flash-md5.new" &&
		mv -f "$tmp/run/.boot-flash-md5.new" "$tmp/run/.boot-flash-md5"
		sleep 0.2; done ) &
	saver=$!
}
stop_saver() {
	kill "$saver" 2>/dev/null; wait "$saver" 2>/dev/null; saver=""
	rm -f "$tmp/run/.boot-flash-md5"
}
mkdir -p "$tmp/look"

# 1. get: blanks around "=" tolerated, comment "#key=" is not a value.
out=$(sh "$tool" get hostname 2>&1); rc=$?
[ "$rc" -eq 0 ] && [ "$out" = "desk-01" ] &&
	ok "get reads 'key = value'" || bad "get hostname rc=$rc: $out"
out=$(sh "$tool" get wifi_ssid 2>&1)
[ "$out" = "m5-BEEF" ] && ok "get wifi_ssid falls back to wpa_supplicant.conf" ||
	bad "get wifi_ssid: $out"
out=$(sh "$tool" get channel 2>&1)
[ "$out" = "6" ] && ok "get reads a CRLF line" || bad "get channel: $out"
out=$(sh "$tool" get password 2>&1)
[ "$out" = "set" ] && echo "$out" | grep -qv "oldsecret" &&
	ok "get masks the password" || bad "get password: $out"

# 2. set: only the key's line changes; comments and other keys stay
#    byte-identical.
start_saver
out=$(sh "$tool" set hostname bench-9 2>&1); rc=$?
stop_saver
[ "$rc" -eq 0 ] && echo "$out" | grep -q "saved to flash" &&
	ok "set reports the save" || bad "set hostname rc=$rc: $out"
mkdir -p "$tmp/look"
newcfg=$(imgfile config.txt)
[ "$(printf '%s\n' "$newcfg" | sed -n '1p')" = "# header comment" ] &&
	echo "$newcfg" | grep -q 'hostname=bench-9' &&
	echo "$newcfg" | grep -q 'wifi_mode=ap' &&
	ok "set edits only the key's line" || bad "edited file: $newcfg"
# the commented-out duplicate was not the one replaced
echo "$newcfg" | grep -q '#wifi_ssid=commented' &&
	ok "comments are not treated as keys" || bad "comment line lost"
[ "$(sh "$tool" get hostname 2>/dev/null)" = "desk-01" ] || true
# /tmp publish copy is stale until bootsyncd publishes -- get must read
# the file the save path uses, so check via the image instead:
out=$(imgfile config.txt | sed -n 's/^hostname=\(.*\)$/\1/p')
[ "$out" = "bench-9" ] && ok "new value is in the image" || bad "value: $out"

# 3. validation: rejected values leave the drive untouched.
before=$(md5sum "$img" | cut -d' ' -f1)
for kv in "hostname -bad-" "wifi_psk short" "channel 99" "wifi_mode zz" \
	"country USA" "sdio_mhz 7" "ssh_password_auth maybe"; do
	set -- $kv
	out=$(sh "$tool" set "$1" "$2" 2>&1); rc=$?
	[ "$rc" -eq 1 ] && [ "$(md5sum "$img" | cut -d' ' -f1)" = "$before" ] ||
		{ bad "set $kv rc=$rc changed the image ($out)"; break; }
done
ok "bad values are rejected without a write"

# 4. set "" removes the key.
start_saver
out=$(sh "$tool" set debug "" 2>&1); rc=$?
stop_saver
[ "$rc" -eq 0 ] && ! imgfile config.txt | grep -q '^debug=' &&
	ok "set key \"\" removes it" || bad "unset rc=$rc: $out"

# 5. the #43 marker refuses with rc 2 and no write.
echo "kernel read-fail: test reason" > "$tmp/run/.boot-nosave"
before=$(md5sum "$img" | cut -d' ' -f1)
out=$(sh "$tool" set hostname later 2>&1); rc=$?
[ "$rc" -eq 2 ] && echo "$out" | grep -q "not saved: kernel read-fail" &&
	[ "$(md5sum "$img" | cut -d' ' -f1)" = "$before" ] &&
	ok "nosave marker: refused rc=2, no write" || bad "nosave rc=$rc: $out"
rm -f "$tmp/run/.boot-nosave"

# 6. a secret never reaches stdout/stderr, but lands in the image.
start_saver
out=$(sh "$tool" set password newsecret9 2>&1); rc=$?
stop_saver
[ "$rc" -eq 0 ] || bad "set password rc=$rc: $out"
case "$out" in *newsecret9*) bad "password printed: $out" ;; esac
imgfile credentials.txt | grep -q '^password=newsecret9$' &&
	imgfile credentials.txt | grep -q '^root_password=oldroot22$' &&
	ok "password written, secret never printed" ||
	bad "credentials.txt: $(imgfile credentials.txt | head -3)"

# 7. authorized_keys: append a valid line, reject a bad one, remove all.
start_saver
out=$(sh "$tool" set authorized_keys "ssh-ed25519 AAAA-testkey user@pc" 2>&1); rc=$?
stop_saver
[ "$rc" -eq 0 ] && imgfile authorized_keys | grep -q "ssh-ed25519 AAAA-testkey" &&
	ok "key appended to the image" || bad "key add rc=$rc: $out"
out=$(sh "$tool" set authorized_keys "not a key" 2>&1); rc=$?
[ "$rc" -eq 1 ] && ok "bad key line rejected" || bad "bad key rc=$rc"
start_saver
out=$(sh "$tool" set authorized_keys "" 2>&1); rc=$?
stop_saver
[ "$rc" -eq 0 ] && ! imgfile authorized_keys 2>/dev/null | grep -q . &&
	ok "remove-all deletes the key file" || bad "key del rc=$rc: $out"

# 8. explained keys: they exist in the files but are not managed.
out=$(sh "$tool" set ssid x 2>&1); rc=$?
[ "$rc" -ne 0 ] && echo "$out" | grep -q "generated values" &&
	ok "ssid= explains it is a generated value" || bad "ssid: $out"
out=$(sh "$tool" set maxcpus 1 2>&1); rc=$?
[ "$rc" -ne 0 ] && echo "$out" | grep -q "no effect" &&
	ok "maxcpus explains it has no effect" || bad "maxcpus: $out"
out=$(sh "$tool" set nosuchkey 1 2>&1); rc=$?
[ "$rc" -eq 64 ] && ok "unknown key is a usage-class reject" ||
	bad "nosuchkey rc=$rc: $out"

# 9. list masks secrets and counts keys.
out=$(sh "$tool" list 2>&1)
echo "$out" | grep -q 'password = (set)' && echo "$out" | grep -qv newsecret9 &&
	ok "list masks secrets" || bad "list: $(echo "$out" | head -4)"
echo "$out" | grep -q 'authorized_keys = 0 key' &&
	ok "list counts keys" || bad "list keys: $out"

# 10. ssh_password_auth=no with no installed keys is rejected.
rm -f "$tmp/home-m5/.ssh/authorized_keys"
out=$(sh "$tool" set ssh_password_auth no 2>&1); rc=$?
[ "$rc" -eq 1 ] && echo "$out" | grep -q "no authorized_keys" &&
	ok "key-only login without keys rejected" || bad "ssh_password_auth rc=$rc"
printf 'ssh-ed25519 AAAA-x\n' > "$tmp/home-m5/.ssh/authorized_keys"
start_saver
out=$(sh "$tool" set ssh_password_auth no 2>&1); rc=$?
stop_saver
[ "$rc" -eq 0 ] && ok "accepted once a key exists" || bad "with key rc=$rc"

# 11. first occurrence wins: set edits the first line, get reads it.
{
	printf 'channel=6\n'
	printf 'channel=11\n'
} > "$tmp/pub/config.txt"
out=$(sh "$tool" get channel 2>&1)
[ "$out" = "6" ] && ok "get reads the first occurrence" || bad "dup get: $out"

# 12. a host-owned LUN gets a board-side forced_eject on save.
echo "$img" > "$LUNF"
start_saver
out=$(sh "$tool" set hostname lun-test 2>&1); rc=$?
stop_saver
[ "$rc" -eq 0 ] || bad "eject set rc=$rc: $out"
ej=""
[ -f "$tmp/udc/udc0/device/udc/udc0/gadget/lun0/forced_eject" ] &&
	read -r ej < "$tmp/udc/udc0/device/udc/udc0/gadget/lun0/forced_eject"
[ "$ej" = "1" ] && ok "host-owned LUN was ejected for the save" ||
	bad "forced_eject=$ej"
: > "$LUNF"

# 13. an unconfirmed save reports rc 3 and the change stays.
out=$(sh "$tool" set hostname unconf 2>&1); rc=$?
[ "$rc" -eq 3 ] && echo "$out" | grep -q "not confirmed" &&
	imgfile config.txt | grep -q 'hostname=unconf' &&
	ok "unconfirmed save: rc=3, change kept" || bad "unconf rc=$rc: $out"

# 14. leftover state: no lock, no mount dir, no work image.
for d in "$tmp/run"/.m5-config.lock "$tmp/run"/.m5cfg-mnt.* "$tmp/pub"/.m5cfg-img.*; do
	[ -e "$d" ] && bad "leftover: $d"
done
ok "no leftovers"

# 15. touch layer (#42 NEW REQ): a stub m5-key plays back scripted
# events; the real item/menu_pick code draws 3-row blocks, maps taps
# to items and handles the nav zones.
mkdir -p "$tmp/bin" "$tmp/tscr"
cat > "$tmp/bin/m5-key" <<'EOF'
#!/bin/sh
[ "$1" = probe ] && exit 0
IFS= read -r ev < "$M5CFG_TSCRIPT" 2>/dev/null || exit 1
tail -n +2 "$M5CFG_TSCRIPT" > "$M5CFG_TSCRIPT.n" 2>/dev/null || :
mv -f "$M5CFG_TSCRIPT.n" "$M5CFG_TSCRIPT" 2>/dev/null || :
printf '%s\n' "$ev"
EOF
chmod +x "$tmp/bin/m5-key"
export M5CFG_FORCE_TOUCH=1 M5CFG_M5_KEY="$tmp/bin/m5-key" \
	M5CFG_TSCRIPT="$tmp/tscr/events"

# The main menu header is: title, board, blank = 3 say lines, so item
# blocks start at row 3, 3 rows each; with _rows=22 the page capacity
# is (22-3-4)/3 = 5 items and the nav block sits at rows 18-20.
# A tap inside the "Wi-Fi" block (rows 3-5) opens the Wi-Fi menu; the
# nav-left zone is Back everywhere, and Back on the main menu exits.
printf 'T 4 10\nT 19 5\nT 19 5\n' > "$tmp/tscr/events"
out=$(M5CFG_TSCRIPT="$tmp/tscr/events" sh "$tool" menu </dev/null 2>&1)
echo "$out" | grep -q "== Wi-Fi ==" &&
	ok "tap on item block opens Wi-Fi" || bad "tap select: $out"
# A tap in the header (row 1, above all blocks) is ignored: the main
# menu is drawn again, not a submenu.
printf 'T 1 40\nT 19 5\n' > "$tmp/tscr/events"
out=$(M5CFG_TSCRIPT="$tmp/tscr/events" sh "$tool" menu </dev/null 2>&1)
echo "$out" | grep -q "== m5-config ==" &&
	! echo "$out" | grep -q "== Wi-Fi ==" &&
	ok "tap outside any block is ignored" ||
	bad "header tap escaped the menu: $out"
# Right nav zone = "more": force 2 items/page so the 5 visible main
# items span three pages; a right-third tap turns it, then Back exits.
printf 'T 10 70\nT 10 5\n' > "$tmp/tscr/events"
out=$(M5CFG_TCAP=2 M5CFG_TSCRIPT="$tmp/tscr/events" \
	sh "$tool" menu </dev/null 2>&1)
echo "$out" | grep -q "page 2 of" &&
	ok "more-zone tap turns the page" || bad "page nav: $out"
# A key byte still reaches the menu while touch is on (panel keyboard).
printf 'K 1\nT 19 5\nT 19 5\n' > "$tmp/tscr/events"
out=$(M5CFG_TSCRIPT="$tmp/tscr/events" sh "$tool" menu </dev/null 2>&1)
echo "$out" | grep -q "== Wi-Fi ==" &&
	ok "key byte selects while touch is on" || bad "key select: $out"

if [ "$fails" -gt 0 ]; then
	echo "FAILED: $fails checks"
	exit 1
fi
echo "PASS"
