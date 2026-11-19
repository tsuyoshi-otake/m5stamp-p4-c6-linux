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
# events; the real item/menu_pick code draws the two-column 80x22
# grid, maps taps to items and handles the nav zones.  Layout (#44):
# header rows 0-2, item buttons at rows 3-18 in two 40-col halves on
# a 4-row pitch (slot i -> top 3+(i/2)*4, col (i%2)*40; 3 colour rows
# plus a gap), message strip row 18, nav rows 19-21 (col<40 back/exit,
# 40-59 prev, 60-79 more).
mkdir -p "$tmp/bin" "$tmp/tscr"
cat > "$tmp/bin/m5-key" <<'EOF'
#!/bin/sh
[ "$1" = probe ] && exit 0
# B33: when M5CFG_BLWATCH names a file, append its content to
# <file>.log before each event -- the value the previous event left
# behind, which makes a live-preview write sequence observable.
[ -n "$M5CFG_BLWATCH" ] &&
	cat "$M5CFG_BLWATCH" >> "$M5CFG_BLWATCH.log" 2>/dev/null
IFS= read -r ev < "$M5CFG_TSCRIPT" 2>/dev/null || exit 1
tail -n +2 "$M5CFG_TSCRIPT" > "$M5CFG_TSCRIPT.n" 2>/dev/null || :
mv -f "$M5CFG_TSCRIPT.n" "$M5CFG_TSCRIPT" 2>/dev/null || :
# 'S <secs>' blocks inside m5-key: the caller then sits in wait(),
# the state a signal test needs to hit.  TERM/INT signal the caller
# like m5-key-log does (the parent is mid-wait -> its trap fires).
case "$ev" in
	S\ *)	sleep "${ev#S }"; exit 1 ;;
	INT)	kill -INT "$PPID" 2>/dev/null; sleep 1; exit 0 ;;
	TERM)	kill -TERM "$PPID" 2>/dev/null; sleep 1; exit 0 ;;
esac
printf '%s\n' "$ev"
EOF
chmod +x "$tmp/bin/m5-key"
export M5CFG_FORCE_TOUCH=1 M5CFG_M5_KEY="$tmp/bin/m5-key" \
	M5CFG_TSCRIPT="$tmp/tscr/events"

# A tap inside the "Wi-Fi" button (slot 0: rows 3-6, cols 0-39) opens
# the Wi-Fi menu; nav-left is Back everywhere, and on the main menu it
# is labelled [exit] and exits.
printf 'T 4 10\nT 20 10\nT 20 10\n' > "$tmp/tscr/events"
out=$(M5CFG_TSCRIPT="$tmp/tscr/events" sh "$tool" menu </dev/null 2>&1)
echo "$out" | grep -q "== Wi-Fi ==" &&
	ok "tap on item block opens Wi-Fi" || bad "tap select: $out"
echo "$out" | grep -q "exit ]" &&
	ok "main menu nav says [exit]" || bad "nav label: $out"
# The right-hand column: item 2 (System) lives at rows 3-6, col 40-79.
printf 'T 4 50\nT 20 10\nT 20 10\n' > "$tmp/tscr/events"
out=$(M5CFG_TSCRIPT="$tmp/tscr/events" sh "$tool" menu </dev/null 2>&1)
echo "$out" | grep -q "== System ==" &&
	ok "tap in the right column opens System" || bad "col2: $out"
# A tap in the header (row 1, above all buttons) is ignored: the menu
# redraws and keeps waiting; a nav tap then leaves.
printf 'T 1 40\nT 20 10\n' > "$tmp/tscr/events"
out=$(M5CFG_TSCRIPT="$tmp/tscr/events" sh "$tool" menu </dev/null 2>&1)
echo "$out" | grep -q "== m5-config ==" &&
	! echo "$out" | grep -q "== Wi-Fi ==" &&
	ok "tap outside any block is ignored" ||
	bad "header tap escaped the menu: $out"
# ... and a dead-zone tap triggers a FULL repaint: the title is
# emitted a second time before the back tap leaves.
printf 'T 1 40\nT 20 10\n' > "$tmp/tscr/events"
out=$(M5CFG_TSCRIPT="$tmp/tscr/events" sh "$tool" menu </dev/null 2>&1)
[ "$(echo "$out" | grep -o '== m5-config ==' | wc -l)" -ge 2 ] &&
	ok "dead-zone tap causes a full redraw" ||
	bad "no redraw after dead tap: $out"
# Pager: force 2 items/page so the 6 visible main items span three
# pages; [more >] (col 60-79) turns, [< prev] (col 40-59) turns back.
printf 'T 20 65\nT 20 45\nT 20 10\n' > "$tmp/tscr/events"
out=$(M5CFG_TPP=2 M5CFG_TSCRIPT="$tmp/tscr/events" \
	sh "$tool" menu </dev/null 2>&1)
echo "$out" | grep -q " 2/3" && echo "$out" | grep -q " 1/3" &&
	ok "more/prev zones turn the page" || bad "page nav: $out"
# A key byte still reaches the menu while touch is on (panel keyboard).
printf 'K 1\nT 20 10\nT 20 10\n' > "$tmp/tscr/events"
out=$(M5CFG_TSCRIPT="$tmp/tscr/events" sh "$tool" menu </dev/null 2>&1)
echo "$out" | grep -q "== Wi-Fi ==" &&
	ok "key byte selects while touch is on" || bad "key select: $out"

# 16. touch dead ends: a text field shows the keyboard note plus a
# [back] block instead of a prompt; a tap leaves and returns to the
# menu.  (menu_set -> ttext)
printf 'T 4 50\nT 4 10\nT 20 10\nT 20 10\n' > "$tmp/tscr/events"
out=$(M5CFG_TSCRIPT="$tmp/tscr/events" sh "$tool" menu </dev/null 2>&1)
echo "$out" | grep -q "needs a keyboard" &&
	ok "text field shows needs-a-keyboard + back" ||
	bad "ttext: $out"
# ... and a typed line still reaches it through the K-byte path.
printf 'T 4 50\nT 4 10\nK h\nT 20 10\nT 20 10\n' > "$tmp/tscr/events"
start_saver
out=$(printf 'ost-9\n' | M5CFG_TSCRIPT="$tmp/tscr/events" \
	sh "$tool" menu 2>&1)
stop_saver
imgfile config.txt | grep -q "hostname=host-9" &&
	ok "K byte + typed rest still enters the value" || bad "ttext kbd: $out"
# a chooser block picks the value by tap: Wi-Fi item 5 (PMF) is slot 4
# (rows 11-14, col 0-39); inside tpick "0 1" value "1" is slot 1.
printf 'T 4 10\nT 12 10\nT 4 50\nT 20 10\nT 20 10\n' > "$tmp/tscr/events"
start_saver
out=$(M5CFG_TSCRIPT="$tmp/tscr/events" sh "$tool" menu </dev/null 2>&1)
stop_saver
imgfile config.txt | grep -q 'wifi_pmf=1' &&
	ok "chooser value picked by tap" || bad "tpick: $out"

# 17. REVIEW 1 group B: validation fixes.
before=$(md5sum "$img" | cut -d' ' -f1)
for kv in "hostname ab!!c" "hostname -x" "hostname x-" \
	"lp_period s" "lp_period ms" "lp_period 12345678901s" \
	"lp_period 2h"; do
	set -- $kv
	out=$(sh "$tool" set "$1" "$2" 2>&1); rc=$?
	[ "$rc" -eq 1 ] && [ "$(md5sum "$img" | cut -d' ' -f1)" = "$before" ] ||
		{ bad "set $kv rc=$rc changed the image ($out)"; break; }
done
ok "group-B rejects: bad hostname/lp_period"
# leading/trailing blanks on secrets are lock-outs and rejected.
_n=0
for spec in "wifi_psk| lead8ch" "password|8trailch " "wifi_ssid| lead"; do
	_k=${spec%%|*}; _v=${spec#*|}
	out=$(sh "$tool" set "$_k" "$_v" 2>&1); rc=$?
	[ "$rc" -eq 1 ] || { _n=1; bad "blank '$_v' rc=$rc ($out)"; }
done
[ "$(md5sum "$img" | cut -d' ' -f1)" = "$before" ] && [ "$_n" -eq 0 ] &&
	ok "edge blanks rejected, image untouched" || bad "edge blanks"
start_saver
for kv in "hostname ab" "hostname m5"; do
	set -- $kv
	out=$(sh "$tool" set "$1" "$2" 2>&1); rc=$?
	[ "$rc" -eq 0 ] || { bad "set $kv rc=$rc ($out)"; break; }
done
stop_saver
ok "group-B accepts: m5/ab hostnames"
# lp_period accepts go through 'set' with the lp_core sysfs seam
# enabled (the key is feature-gated to boards that have it).
mkdir -p "$tmp/sysboard/lp_core"
start_saver
for v in 0 500ms 1s 1m 30m 1h; do
	out=$(M5CFG_SYS_BOARD="$tmp/sysboard" sh "$tool" set lp_period "$v" 2>&1)
	rc=$?
	[ "$rc" -eq 0 ] || { bad "lp_period $v rc=$rc ($out)"; break; }
done
for v in s ms 12345678901s 2h 90m 3601s; do
	out=$(M5CFG_SYS_BOARD="$tmp/sysboard" sh "$tool" set lp_period "$v" 2>&1)
	rc=$?
	[ "$rc" -eq 1 ] || { bad "lp_period $v rc=$rc ($out)"; break; }
done
stop_saver
ok "lp_period mirrors the S05tmpfs range (0 .. 1h)"
# leading/trailing blanks rejected with a clear message
out=$(sh "$tool" set password " lead9char" 2>&1)
echo "$out" | grep -q "blank" && ok "edge blank message" || bad "blank: $out"
# editor_set keeps the CR on a CRLF line
start_saver
sh "$tool" set channel 7 >/dev/null 2>&1
stop_saver
# od -c sees the real bytes (MSYS grep textifies its stdin, hiding CR)
imgfile config.txt | od -c | grep -q '7  \\r' &&
	ok "CRLF line keeps its CR" || bad "CR lost: $(imgfile config.txt | grep channel | od -c | head -1)"

# 18. REVIEW 1 group C: a rejected value keeps the menu alive.
# (plain menu: touch seam off so ask() reads the piped lines)
printf '2\n1\n-bad-\nok-host\n\n0\n0\n' > "$tmp/menuin"
start_saver
out=$(env -u M5CFG_FORCE_TOUCH -u M5CFG_TSCRIPT \
	sh "$tool" menu < "$tmp/menuin" 2>&1); rc=$?
stop_saver
echo "$out" | grep -q "rejected" && imgfile config.txt | grep -q 'hostname=ok-host' &&
	ok "rejected value: menu continues, next value saved" ||
	bad "menu died on reject rc=$rc: $(echo "$out" | tail -3)"
grep -n "trap '_cleanup; exit 130'" "$tool" >/dev/null &&
	ok "signal traps exit after cleanup" || bad "trap does not exit"

# 19. REVIEW 2: secrets typed through the touch path are never echoed.
# System (main slot 1) -> "m5 password" (slot 1: rows 3-6, col 40) ->
# ttext secret -> K n + typed rest; the value shows only as '*' and
# lands in the image.
printf 'T 4 50\nT 4 50\nK n\nT 20 10\nT 20 10\nT 20 10\n' > "$tmp/tscr/events"
start_saver
out=$(printf 'ewsecret99\n' | M5CFG_TSCRIPT="$tmp/tscr/events" \
	sh "$tool" menu 2>&1)
stop_saver
case "$out" in *newsecret99*) bad "touch secret echoed: $out" ;; esac
echo "$out" | grep -q 'value: ' && echo "$out" | grep -q '\*\*\*' &&
	imgfile credentials.txt | grep -q '^password=newsecret99$' &&
	ok "touch secret: stars shown, value saved, never echoed" ||
	bad "touch secret: $out"

# 20. '?' is a real first char now (K ? is a literal, not a control
# token): a password may start with '?'.
printf 'T 4 50\nT 4 50\nK ?\nT 20 10\nT 20 10\nT 20 10\n' > "$tmp/tscr/events"
start_saver
out=$(printf 'abc12345\n' | M5CFG_TSCRIPT="$tmp/tscr/events" \
	sh "$tool" menu 2>&1)
stop_saver
imgfile credentials.txt | grep -q '^password=?abc12345$' &&
	ok "K ? starts a password value" || bad "K ?: $out"

# 21. Result messages live in the message strip (row 19 = MSGTOP+1),
# not under the nav block: a touch set shows "saving ..." then
# "saved to flash" at the parked cursor, and the prompt sat at row 19.
printf 'T 4 50\nT 4 10\nK b\nT 20 10\nT 20 10\nT 20 10\n' > "$tmp/tscr/events"
start_saver
out=$(printf 'ox-1\n' | M5CFG_TSCRIPT="$tmp/tscr/events" \
	sh "$tool" menu 2>&1)
stop_saver
echo "$out" | grep -q 'saving \.\.\.' && echo "$out" | grep -q 'saved to flash' &&
	echo "$out" | grep -q "$(printf '\033\[19;1H')value: " &&
	ok "results print in the message area" || bad "msg area: $out"
imgfile config.txt | grep -q 'hostname=box-1' &&
	ok "touch set still saves" || bad "touch set"

# 22. K enter/K bs in tadjust: Wi-Fi -> channel (slot 3, rows 7-10 col
# 40) -> type 1 2, erase, 3, enter -> channel=13.
printf 'T 4 10\nT 8 50\nK 1\nK 2\nK bs\nK 3\nK enter\nT 20 10\nT 20 10\nT 20 10\n' \
	> "$tmp/tscr/events"
start_saver
out=$(M5CFG_TSCRIPT="$tmp/tscr/events" sh "$tool" menu </dev/null 2>&1)
stop_saver
imgfile config.txt | grep -q '^channel=13$' &&
	ok "tadjust: digits + K bs + K enter" || bad "tadjust kbd: $out"

# 23. K enter in tpick cancels without picking: wifi_mode stays ap.
printf 'T 4 10\nT 4 10\nK enter\nT 20 10\nT 20 10\nT 20 10\n' > "$tmp/tscr/events"
out=$(M5CFG_TSCRIPT="$tmp/tscr/events" sh "$tool" menu </dev/null 2>&1)
imgfile config.txt | grep -q '^wifi_mode=ap$' &&
	ok "K enter in tpick = back, no pick" || bad "tpick enter"

# 24. Buttons are visible: every block draws in the redefined button
# colour (slot 4 = #3b4252), and the Nord palette is set with ESC ] P
# on entry and restored with ESC ] R on the way out.
printf 'T 20 10\n' > "$tmp/tscr/events"
out=$(M5CFG_TSCRIPT="$tmp/tscr/events" sh "$tool" menu </dev/null 2>&1)
echo "$out" | grep -q "$(printf '\033\[37;44m')" &&
	ok "blocks draw as palette-colour buttons" || bad "no buttons"
echo "$out" | grep -q "$(printf '\033\]P02e3440')" &&
	ok "Nord palette is set (ESC ] P)" || bad "palette not set"
echo "$out" | grep -q "$(printf '\033\]R')" &&
	ok "palette restored on exit (ESC ] R)" || bad "palette not restored"
# the tapped button briefly changes to the tapped colour (#5e81ac)
printf 'T 4 10\nT 20 10\n' > "$tmp/tscr/events"
out=$(M5CFG_TSCRIPT="$tmp/tscr/events" sh "$tool" menu </dev/null 2>&1)
echo "$out" | grep -q "$(printf '\033\[37;45m')" &&
	ok "tapped button flashes in the tapped colour" || bad "no tap flash"

# 25. Bluetooth: stub ble-kbd with two same-named keyboards.  Blocks
# carry "NAME_(..:EE:FF)" labels; the tap resolves by slot index --
# the SECOND keyboard's address must reach pair.  The pairing screen
# shows the bold passkey line.
cat > "$tmp/bin/ble-kbd" <<'EOF'
#!/bin/sh
echo "ble-kbd $*" >> "$M5CFG_BLELOG"
case "$1" in
scan|list) cat "$M5CFG_BLE_IN" ;;
pair) printf 'pairing %s ...\n  type 654321 on the keyboard, then Enter\npaired + bonded\n' "$2" ;;
forget) printf 'forgotten\n' ;;
esac
EOF
chmod +x "$tmp/bin/ble-kbd"
printf '  AA:BB:CC:DD:EE:01 type=1 rssi=-50 [HID] name=Kbd One\n' \
	 > "$tmp/ble-in"
printf '  AA:BB:CC:DD:EE:02 type=1 rssi=-60 [HID] name=Kbd One\n' \
	>> "$tmp/ble-in"
: > "$tmp/blelog"
# "Bluetooth keyboard" is main slot 3 (rows 7-10, col 40) when the
# stub makes the feature present; the 2nd scan entry is tpick slot 1.
printf 'T 8 50\nT 4 10\nT 4 50\nT 20 10\nT 20 10\nT 20 10\n' > "$tmp/tscr/events"
out=$(M5CFG_BLE_KBD="$tmp/bin/ble-kbd" M5CFG_BLELOG="$tmp/blelog" \
	M5CFG_BLE_IN="$tmp/ble-in" M5CFG_TSCRIPT="$tmp/tscr/events" \
	sh "$tool" menu </dev/null 2>&1)
echo "$out" | grep -q 'EE:01' && echo "$out" | grep -q 'EE:02' &&
	ok "same-named keyboards get distinct labels" || bad "ble labels: $out"
grep -q 'pair AA:BB:CC:DD:EE:02' "$tmp/blelog" &&
	ok "tap resolved by slot index (2nd addr)" ||
	bad "slot resolve: $(cat "$tmp/blelog")"
echo "$out" | grep -q 'pairing Kbd_One (AA:BB:CC:DD:EE:02)' &&
	echo "$out" | grep -q "$(printf '\033\[1m')  type 654321" &&
	ok "pairing screen + bold passkey line" || bad "pairing screen: $out"

# 26. Status is a full text page: >19 lines page with [ more > ], the
# nav block sits below, and a marker past row 19 shows on page 2.
{
	i=1; while [ "$i" -le 25 ]; do echo "status line $i"; i=$((i + 1)); done
} > "$tmp/status.txt"
cat > "$tmp/bin/sysinfo" <<'EOF'
#!/bin/sh
i=1; while [ "$i" -le 10 ]; do echo "sysinfo row $i MARKER$i"; i=$((i + 1)); done
EOF
chmod +x "$tmp/bin/sysinfo"
cat > "$tmp/bin/usb-lun" <<'EOF'
#!/bin/sh
[ "$1" = state ] && echo host-owned
EOF
chmod +x "$tmp/bin/usb-lun"
echo valid > "$tmp/sysboard/bootfat"
# Status is main slot 4 (rows 11-14, col 0); [more] twice -> page 3.
printf 'T 12 10\nT 20 65\nT 20 65\nT 20 10\nT 20 10\n' > "$tmp/tscr/events"
out=$(M5CFG_STATUS_TXT="$tmp/status.txt" M5CFG_SYSINFO="$tmp/bin/sysinfo" \
	M5CFG_USB_LUN="$tmp/bin/usb-lun" M5CFG_SYS_BOARD="$tmp/sysboard" \
	M5CFG_TSCRIPT="$tmp/tscr/events" sh "$tool" menu </dev/null 2>&1)
echo "$out" | grep -q 'status line 25' && echo "$out" | grep -q 'MARKER10' &&
	echo "$out" | grep -q 'more 1/3' &&
	echo "$out" | grep -q 'bootfat: valid' && echo "$out" | grep -q 'host-owned' &&
	ok "status pages: 19-line pages + more nav" || bad "status page: $out"

# 27. REVIEW 3: ttext secret mode calls `stty -echo` BEFORE the first
# m5-key call (m5-key then saves/restores a termios that already has
# ECHO off, so a fast second char cannot echo in the vfork+exec gap)
# and `stty echo` after the last read on every exit path -- value
# typed, back tap, m5-key failure, and the INT/TERM trap.
cat > "$tmp/bin/stty" <<'EOF'
#!/bin/sh
echo "STTY $*" >> "$M5LOG"
exit 0
EOF
cat > "$tmp/bin/m5-key-log" <<'EOF'
#!/bin/sh
echo "M5KEY" >> "$M5LOG"
[ "$1" = probe ] && exit 0
IFS= read -r ev < "$M5CFG_TSCRIPT" 2>/dev/null || exit 1
tail -n +2 "$M5CFG_TSCRIPT" > "$M5CFG_TSCRIPT.n" 2>/dev/null || :
mv -f "$M5CFG_TSCRIPT.n" "$M5CFG_TSCRIPT" 2>/dev/null || :
[ "$ev" = "INT" ] && { kill -INT "$PPID" 2>/dev/null; sleep 1; exit 0; }
[ "$ev" = "TERM" ] && { kill -TERM "$PPID" 2>/dev/null; sleep 1; exit 0; }
printf '%s\n' "$ev"
EOF
chmod +x "$tmp/bin/stty" "$tmp/bin/m5-key-log"

# Between "STTY -echo" and the matching "STTY echo" exactly one M5KEY
# line may appear (the first-byte call); on the typed path the saved
# value proves the read happened before echo came back.
echo_check() { # $1 desc; log is $tmp/m5.log
	off=$(grep -n 'STTY -echo' "$tmp/m5.log" | tail -1 | cut -d: -f1)
	# the ttext restore is the FIRST 'STTY echo' after '-echo'; the
	# session cleanup adds a later safety 'stty echo' of its own.
	on=$(awk -v a="$off" 'NR>a && /STTY echo/ { print NR; exit }' \
		"$tmp/m5.log")
	keys=$(awk -v a="$off" -v b="$on" 'NR>a && NR<b' \
		"$tmp/m5.log" | grep -c M5KEY)
	[ -n "$off" ] && [ -n "$on" ] && [ "$on" -gt "$off" ] &&
		[ "$keys" -eq 1 ] || return 1
	return 0
}

# typed value: System(1) -> m5 password(1) -> K s + stdin "ekret999"
printf 'T 4 50\nT 4 50\nK s\nT 20 10\n' > "$tmp/tscr/events"
: > "$tmp/m5.log"
start_saver
out=$(printf 'ekret999\n' | M5LOG="$tmp/m5.log" M5CFG_STTY="$tmp/bin/stty" \
	M5CFG_M5_KEY="$tmp/bin/m5-key-log" M5CFG_TSCRIPT="$tmp/tscr/events" \
	sh "$tool" menu 2>&1)
stop_saver
echo_check && imgfile credentials.txt | grep -q '^password=sekret999$' &&
	ok "stty order: -echo before first m5-key, echo after read" ||
	{ bad "stty order typed: $(cat "$tmp/m5.log")"; }

# back tap: same entry, T instead of a key -> STTY echo, nothing saved
printf 'T 4 50\nT 4 50\nT 20 10\nT 20 10\n' > "$tmp/tscr/events"
: > "$tmp/m5.log"
out=$(M5LOG="$tmp/m5.log" M5CFG_STTY="$tmp/bin/stty" \
	M5CFG_M5_KEY="$tmp/bin/m5-key-log" M5CFG_TSCRIPT="$tmp/tscr/events" \
	sh "$tool" menu </dev/null 2>&1)
echo_check && imgfile credentials.txt | grep -q '^password=sekret999$' &&
	ok "stty echo on back tap" ||
	{ bad "stty order back: $(cat "$tmp/m5.log")"; }

# m5-key failure: events run out inside ttext -> failure exit, echo back
printf 'T 4 50\nT 4 50\n' > "$tmp/tscr/events"
: > "$tmp/m5.log"
out=$(M5LOG="$tmp/m5.log" M5CFG_STTY="$tmp/bin/stty" \
	M5CFG_M5_KEY="$tmp/bin/m5-key-log" M5CFG_TSCRIPT="$tmp/tscr/events" \
	sh "$tool" menu </dev/null 2>&1)
echo_check && ok "stty echo on m5-key failure" ||
	{ bad "stty order eof: $(cat "$tmp/m5.log")"; }

# INT/TERM trap: the stub's 3rd call hits the script with SIGINT mid-
# ttext; the trap must put echo back (rc 130, STTY echo last).
printf 'T 4 50\nT 4 50\nINT\n' > "$tmp/tscr/events"
: > "$tmp/m5.log"
out=$(M5LOG="$tmp/m5.log" M5CFG_STTY="$tmp/bin/stty" \
	M5CFG_M5_KEY="$tmp/bin/m5-key-log" M5CFG_TSCRIPT="$tmp/tscr/events" \
	sh "$tool" menu </dev/null 2>&1)
rc=$?
tail -1 "$tmp/m5.log" | grep -q 'STTY echo' && [ "$rc" -eq 130 ] &&
	ok "INT trap restores echo (rc=130)" ||
	{ bad "stty order int rc=$rc: $(cat "$tmp/m5.log")"; }

# 28. Non-secret ttext never touches stty (plain fields keep normal
# echo): the same field edit through ttext logs no STTY -echo.  (The
# session cleanup's final 'stty echo' safety restore is allowed.)
printf 'T 4 50\nT 4 10\nK b\nT 20 10\nT 20 10\n' > "$tmp/tscr/events"
: > "$tmp/m5.log"
start_saver
out=$(printf 'ox-2\n' | M5LOG="$tmp/m5.log" M5CFG_STTY="$tmp/bin/stty" \
	M5CFG_M5_KEY="$tmp/bin/m5-key-log" M5CFG_TSCRIPT="$tmp/tscr/events" \
	sh "$tool" menu 2>&1)
stop_saver
! grep -q 'STTY -echo' "$tmp/m5.log" &&
	imgfile config.txt | grep -q '^hostname=box-2$' &&
	ok "non-secret ttext calls no stty" ||
	{ bad "non-secret stty: $(cat "$tmp/m5.log")"; }

# 29. REVIEW 1: a save mid-session must NOT drop the session traps or
# the palette -- the save's own cleanup is _drive_clean; _cleanup runs
# once at session end.  So ESC]R is emitted exactly once, after the
# last repaint, and a signal arriving after the save still restores.
escR=$(printf '\033]R'); escB=$(printf '\033[37;44m')
# System(slot 1) -> hostname(slot 0) -> K b + stdin "ox-3" -> saved;
# back to the System menu (which repaints), back to main, exit.
printf 'T 4 50\nT 4 10\nK b\nT 20 10\nT 20 10\n' > "$tmp/tscr/events"
start_saver
out=$(printf 'ox-3\n' | M5CFG_TSCRIPT="$tmp/tscr/events" \
	sh "$tool" menu 2>&1)
stop_saver
imgfile config.txt | grep -q '^hostname=box-3$' ||
	bad "review1 save did not land"
n=$(printf '%s' "$out" | grep -o "$escR" | wc -l)
[ "$n" -eq 1 ] || bad "ESC]R emitted ${n}x"
case "$out" in
	*"$escR"*"$escB"*)
		bad "palette restored before the last repaint" ;;
	*"$escB"*"$escR"*)
		ok "ESC]R exactly once, after the last repaint" ;;
	*) bad "no palette sequence in output" ;;
esac
# The 0600 event file (can hold a secret's first byte) is gone at
# session end, not left in RUN_DIR for the next reader.
ls "$tmp/run"/.m5cfg-ev.* >/dev/null 2>&1 &&
	bad "event file left behind" ||
	ok "event file removed at session end"

# Non-touch mode has no session trap either: a Status visit (which
# runs usb-lun through a scratch file) must still leave no .m5cfg-*.
# Two separate assertions: the visit must actually show the usb-lun
# line, and afterwards RUN_DIR must hold no scratch files.
printf '9\n0\n0\n' > "$tmp/tscr/kbd"
out=$(M5CFG_FORCE_TOUCH= M5CFG_STATUS_TXT="$tmp/status.txt" \
	M5CFG_SYSINFO="$tmp/bin/sysinfo" M5CFG_USB_LUN="$tmp/bin/usb-lun" \
	M5CFG_SYS_BOARD="$tmp/sysboard" sh "$tool" menu \
	< "$tmp/tscr/kbd" 2>&1)
echo "$out" | grep -q "host-owned" ||
	bad "non-touch Status did not show usb-lun state"
if ls "$tmp/run"/.m5cfg-* >/dev/null 2>&1; then
	bad "non-touch Status left scratch files in RUN_DIR"
else
	ok "non-touch Status leaves no scratch files"
fi

# touch_ok accepts only /dev/tty[0-9]*: on the Tab5, /dev/console is
# the USB gadget serial (console=tty0 console=ttyGS1), not the panel
# -- a run there must stay plain.  A fake tty shadows the real one.
cat > "$tmp/bin/tty" <<'EOF'
#!/bin/sh
echo /dev/console
EOF
chmod +x "$tmp/bin/tty"
printf '0\n' > "$tmp/tscr/kbd"
out=$(M5CFG_FORCE_TOUCH= sh "$tool" menu < "$tmp/tscr/kbd" 2>&1)
echo "$out" | grep -q "== m5-config ==" ||
	bad "menu did not render with tty=/dev/console"
echo "$out" | grep -q "$(printf '\033\]P')" &&
	bad "/dev/console took the touch path" ||
	ok "/dev/console stays plain mode"

# A TERM after the save still runs the session cleanup: ESC]R once,
# rc 130.  (Before the split the save had already cleared the traps,
# so this died on the default disposition with no restore.)
printf 'T 4 50\nT 4 10\nK b\nTERM\n' > "$tmp/tscr/events"
: > "$tmp/m5.log"
start_saver
out=$(printf 'ox-4\n' | M5LOG="$tmp/m5.log" M5CFG_STTY="$tmp/bin/stty" \
	M5CFG_M5_KEY="$tmp/bin/m5-key-log" M5CFG_TSCRIPT="$tmp/tscr/events" \
	sh "$tool" menu 2>&1)
rc=$?
stop_saver
n=$(printf '%s' "$out" | grep -o "$escR" | wc -l)
[ "$rc" -eq 130 ] && [ "$n" -eq 1 ] &&
	ok "signal after a save still restores the palette" ||
	bad "signal after save rc=$rc ESC]R=$n"

# 30. B25 NOMMU lint: no $( ) inside the touch event/redraw path.
# Each $( ) forks a whole second hush (~480 KB contiguous on NOMMU);
# in the input wait that is the difference between two idle processes
# (m5-config + m5-key) and alloc failures.  Arithmetic $(( )) is fine;
# the pattern below only flags a $( that does not open another (.
_lint_fns="menu_pick tpick tconfirm tadjust ttext tpage pause item \
_tblock _navdraw _tflash _tget say msg _tmsg nosave_note scr_clear \
menu_wifi menu_system menu_datetime menu_display menu_sound \
menu_ports menu_blekbd menu_power menu_lpcore menu_status menu_main \
status_lines menu_set menu_secret menu_choose menu_adjust menu_keys \
_blescan touch_ok _touch_init _pal_set _pal_restore set_apply trun \
cfg_get eff_file key_file keys_file eff_ssid key_explained lun_file \
do_get is_key_line pwroff_support _cleanup _drive_clean"
_lint_hit=0
for fn in $_lint_fns; do
	awk -v f="$fn" 'index($0, f "()") == 1 { on = 1 }
		on { print }
		on && /^}/ { exit }' "$tool" > "$tmp/fnbody"
	[ -s "$tmp/fnbody" ] || { _lint_hit=1; bad "lint: $fn not found"; continue; }
	grep -nE '\$\([^()]' "$tmp/fnbody" &&
		{ _lint_hit=1; bad "lint: \$( ) inside $fn"; }
	grep -n 'set -- ' "$tmp/fnbody" &&
		{ _lint_hit=1; bad "lint: set -- inside $fn"; }
done
[ "$_lint_hit" -eq 0 ] &&
	ok "lint: no \$( ) or set -- inside the touch event/redraw path" ||
	bad "lint found \$( ) or set -- in touch functions"

# 31. B29: header colours must reach the panel as a real ESC byte, not
# the four characters \033.  say() and the tpage pager pass the colour
# variable through %b so busybox printf interprets its escapes; a %s
# slot would print the backslash text verbatim on the panel.
printf 'T 20 10\n' > "$tmp/tscr/events"
out=$(M5CFG_TSCRIPT="$tmp/tscr/events" sh "$tool" menu </dev/null 2>&1)
! printf '%s' "$out" | grep -qF '\033' ||
	bad "menu emitted literal \\033 text"
printf '%s' "$out" | grep -q "$(printf '\033\[36m')== m5-config ==" &&
	ok "menu header is a real ESC[36m, no literal \\033" ||
	bad "menu header colour: $out"
# the tpage pager paints ==* lines with the same colour slot.
printf 'T 12 10\nT 20 10\n' > "$tmp/tscr/events"
out=$(M5CFG_STATUS_TXT="$tmp/status.txt" M5CFG_SYSINFO="$tmp/bin/sysinfo" \
	M5CFG_USB_LUN="$tmp/bin/usb-lun" M5CFG_SYS_BOARD="$tmp/sysboard" \
	M5CFG_TSCRIPT="$tmp/tscr/events" sh "$tool" menu </dev/null 2>&1)
! printf '%s' "$out" | grep -qF '\033' ||
	bad "status emitted literal \\033 text"
printf '%s' "$out" | grep -q "$(printf '\033\[36m')== Status ==" &&
	ok "status header is a real ESC[36m, no literal \\033" ||
	bad "status header colour: $out"

# 32. B31: a `set -- ${_ev#T }` tap parse clobbered each touch
# helper's own arguments -- tadjust lost MIN/MAX/STEP (arithmetic
# syntax error on the panel), tpick/tconfirm repainted `== <row> ==`
# as the title after any non-selecting tap.  These drive the same
# screens with T events.  (M5CFG_BLDIR stubs the backlight glob so
# the Display item exists at all.)
mkdir -p "$tmp/bl/bl0"
echo 1000 > "$tmp/bl/bl0/max_brightness"
echo 500 > "$tmp/bl/bl0/brightness"
# Display (slot 3 with dsp=1): 50 -5 -5 +5 = 45 -> writes 450.
printf 'T 8 50\nT 4 10\nT 4 10\nT 4 50\nT 8 10\nT 20 10\nT 20 10\n' \
	> "$tmp/tscr/events"
out=$(M5CFG_BLDIR="$tmp/bl" M5CFG_TSCRIPT="$tmp/tscr/events" \
	sh "$tool" menu </dev/null 2>&1)
[ "$(cat "$tmp/bl/bl0/brightness")" = 450 ] &&
	echo "$out" | grep -qF 'set to 45%' &&
	! echo "$out" | grep -qi 'arithmetic' &&
	[ "$(echo "$out" | grep -oF '== brightness ==' | wc -l)" -ge 2 ] &&
	! echo "$out" | grep -qE '== [0-9]+ ==' &&
	echo "$out" | grep -qF '5 .. 100' &&
	ok "tadjust taps: -5 -5 +5 -> 45, title+range intact" ||
	bad "tadjust taps: bl=$(cat "$tmp/bl/bl0/brightness") $out"
# [-] at MIN clamps at 5: REPLY=5 -> writes 50.
echo 50 > "$tmp/bl/bl0/brightness"
printf 'T 8 50\nT 4 10\nT 8 10\nT 20 10\nT 20 10\n' > "$tmp/tscr/events"
out=$(M5CFG_BLDIR="$tmp/bl" M5CFG_TSCRIPT="$tmp/tscr/events" \
	sh "$tool" menu </dev/null 2>&1)
[ "$(cat "$tmp/bl/bl0/brightness")" = 50 ] &&
	echo "$out" | grep -qF 'set to 5%' &&
	! echo "$out" | grep -qi 'arithmetic' &&
	ok "tadjust [-] clamps at MIN" || bad "tadjust MIN: $out"
# [+] at MAX clamps at 100: REPLY=100 -> writes 1000.
echo 1000 > "$tmp/bl/bl0/brightness"
printf 'T 8 50\nT 4 50\nT 8 10\nT 20 10\nT 20 10\n' > "$tmp/tscr/events"
out=$(M5CFG_BLDIR="$tmp/bl" M5CFG_TSCRIPT="$tmp/tscr/events" \
	sh "$tool" menu </dev/null 2>&1)
[ "$(cat "$tmp/bl/bl0/brightness")" = 1000 ] &&
	echo "$out" | grep -qF 'set to 100%' &&
	! echo "$out" | grep -qi 'arithmetic' &&
	ok "tadjust [+] clamps at MAX" || bad "tadjust MAX: $out"
# tconfirm: a dead tap outside yes/no must not turn the question
# into the tapped row number (System -> SSH keys m5 -> remove all).
printf 'T 4 50\nT 12 10\nT 4 50\nT 1 40\nT 4 10\nT 20 10\nT 20 10\nT 20 10\nT 20 10\n' \
	> "$tmp/tscr/events"
start_saver
out=$(M5CFG_TSCRIPT="$tmp/tscr/events" sh "$tool" menu </dev/null 2>&1)
stop_saver
[ "$(echo "$out" | grep -oF '== remove all m5 keys? ==' | wc -l)" -ge 2 ] &&
	! echo "$out" | grep -qE '== [0-9]+ ==' &&
	ok "tconfirm: dead tap redraws the same question" ||
	bad "tconfirm title: $out"
# tpick over two pages: [ more ] then [ back ]; the title after the
# page turn is still the tpick title (9 scan entries > TPP=8).
i=1; : > "$tmp/ble-in"
while [ "$i" -le 9 ]; do
	printf '  AA:BB:CC:DD:EE:%02d type=1 rssi=-50 [HID] name=Kbd %d\n' \
		"$i" "$i" >> "$tmp/ble-in"
	i=$((i + 1))
done
printf 'T 12 10\nT 4 10\nT 20 65\nT 20 10\nT 20 10\nT 20 10\n' \
	> "$tmp/tscr/events"
out=$(M5CFG_BLDIR="$tmp/bl" M5CFG_BLE_KBD="$tmp/bin/ble-kbd" \
	M5CFG_BLELOG="$tmp/blelog" M5CFG_BLE_IN="$tmp/ble-in" \
	M5CFG_TSCRIPT="$tmp/tscr/events" sh "$tool" menu </dev/null 2>&1)
[ "$(echo "$out" | grep -oF '== pair which? ==' | wc -l)" -ge 2 ] &&
	! echo "$out" | grep -qE '== [0-9]+ ==' &&
	ok "tpick: [ more ] redraws the same title" ||
	bad "tpick title: $out"

# 33. B33: brightness follows the taps in tadjust (#42) -- every
# [-]/[+] tap writes the probed backlight at once, [OK] keeps it,
# while [cancel], events-EOF/m5-key loss and a signal all restore
# the value that was current on entry.  The stub appends the file's
# content to <BLWATCH>.log before each event, so log line N is the
# value event N-1's processing left behind.
BLW="$tmp/bl/bl0/brightness"
# [-] [-] [+] [OK]: previews write 450,400,450 in order, [OK] keeps.
echo 500 > "$BLW"; : > "$BLW.log"
printf 'T 8 50\nT 4 10\nT 4 10\nT 4 50\nT 8 10\nT 20 10\nT 20 10\n' \
	> "$tmp/tscr/events"
out=$(M5CFG_BLDIR="$tmp/bl" M5CFG_BLWATCH="$BLW" \
	M5CFG_TSCRIPT="$tmp/tscr/events" sh "$tool" menu </dev/null 2>&1)
[ "$(cat "$BLW")" = 450 ] &&
	echo "$out" | grep -qF 'set to 45%' &&
	[ "$(sed -n '3,5p' "$BLW.log" | tr '\n' ' ')" = "450 400 450 " ] &&
	ok "brightness previews each tap, [OK] keeps it" ||
	bad "preview OK: log=$(tr '\n' ' ' < "$BLW.log") \
bl=$(cat "$BLW") $out"
# [-] [cancel]: the previewed 45% rolls back to the entry value.
echo 500 > "$BLW"; : > "$BLW.log"
printf 'T 8 50\nT 4 10\nT 8 60\nT 20 10\nT 20 10\n' > "$tmp/tscr/events"
out=$(M5CFG_BLDIR="$tmp/bl" M5CFG_BLWATCH="$BLW" \
	M5CFG_TSCRIPT="$tmp/tscr/events" sh "$tool" menu </dev/null 2>&1)
[ "$(cat "$BLW")" = 500 ] &&
	echo "$out" | grep -qF 'unchanged' &&
	[ "$(sed -n '3p' "$BLW.log")" = 450 ] &&
	[ "$(sed -n '4p' "$BLW.log")" = 500 ] &&
	ok "brightness [cancel] restores the entry value" ||
	bad "preview cancel: log=$(tr '\n' ' ' < "$BLW.log") \
bl=$(cat "$BLW") $out"
# Events run out inside tadjust (m5-key EOF): same restore.
echo 500 > "$BLW"; : > "$BLW.log"
printf 'T 8 50\nT 4 10\n' > "$tmp/tscr/events"
out=$(M5CFG_BLDIR="$tmp/bl" M5CFG_BLWATCH="$BLW" \
	M5CFG_TSCRIPT="$tmp/tscr/events" sh "$tool" menu </dev/null 2>&1)
[ "$(cat "$BLW")" = 500 ] &&
	[ "$(sed -n '3p' "$BLW.log")" = 450 ] &&
	[ "$(sed -n '4p' "$BLW.log")" = 500 ] &&
	ok "brightness EOF restores the entry value" ||
	bad "preview EOF: log=$(tr '\n' ' ' < "$BLW.log") \
bl=$(cat "$BLW") $out"
# A signal inside tadjust: the trap restores before exit (rc 130).
echo 500 > "$BLW"; : > "$BLW.log"
printf 'T 8 50\nT 4 10\nTERM\n' > "$tmp/tscr/events"
out=$(M5CFG_BLDIR="$tmp/bl" M5CFG_BLWATCH="$BLW" \
	M5CFG_TSCRIPT="$tmp/tscr/events" sh "$tool" menu </dev/null 2>&1)
rc=$?
[ "$rc" -eq 130 ] &&
	[ "$(cat "$BLW")" = 500 ] &&
	[ "$(sed -n '3p' "$BLW.log")" = 450 ] &&
	ok "brightness signal restores the entry value" ||
	bad "preview signal: rc=$rc log=$(tr '\n' ' ' < "$BLW.log") \
bl=$(cat "$BLW") $out"
# Plain mode is unchanged: the ask path still writes once, on Enter.
printf '4\n70\n\n0\n' > "$tmp/tscr/kbd"
echo 500 > "$BLW"
out=$(env -u M5CFG_FORCE_TOUCH M5CFG_BLDIR="$tmp/bl" \
	sh "$tool" menu < "$tmp/tscr/kbd" 2>&1)
[ "$(cat "$BLW")" = 700 ] &&
	echo "$out" | grep -qF 'set to 70%' &&
	ok "plain mode still writes once, on entry" ||
	bad "plain brightness: bl=$(cat "$BLW") $out"

if [ "$fails" -gt 0 ]; then
	echo "FAILED: $fails checks"
	exit 1
fi
echo "PASS"
