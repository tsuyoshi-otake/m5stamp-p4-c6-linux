#!/bin/sh
# tab5-rails against a fake sysfs gpio tree: the expander keys of
# config.txt (ext5v_en, ext_ant_en on 0x43) and the charger key (CHG_EN,
# 0x44 P7).  The charger must stay off without a battery pack -- a
# charger working into an empty pack node gets the Tab5's radio chip
# power-on reset every few seconds (linux/tab5/charger-no-pack.md).
set -u

repo_root=$(cd -- "$(dirname -- "$0")/../.." && pwd)
tool="$repo_root/linux/m3-lab/rootfs-overlay-tab5/usr/sbin/tab5-rails"
shim="$repo_root/linux/boot-shim/main/tab5_board.c"
tmp=${TMPDIR:-/tmp}/tab5-rails-test.$$
trap 'rm -rf -- "$tmp"' EXIT HUP INT TERM
fails=0
ok() { echo "ok: $1"; }
bad() { echo "FAIL: $1"; fails=$((fails + 1)); }

# 0x43 at 512..519, 0x44 at 520..527; the lines the helper drives are
# already exported, as they are after the first boot-time run.
fresh() {
	rm -rf "$tmp"
	mkdir -p "$tmp/gpio/gpiochip512" "$tmp/gpio/gpiochip520" \
		"$tmp/gpio/gpiochip0" "$tmp/psy"
	echo esp32p4-gpio-bank0 > "$tmp/gpio/gpiochip0/label"
	echo 0 > "$tmp/gpio/gpiochip0/base"
	echo 0-0043 > "$tmp/gpio/gpiochip512/label"
	echo 512 > "$tmp/gpio/gpiochip512/base"
	echo 0-0044 > "$tmp/gpio/gpiochip520/label"
	echo 520 > "$tmp/gpio/gpiochip520/base"
	: > "$tmp/gpio/export"
	for n in 512 514 527; do
		mkdir -p "$tmp/gpio/gpio$n"
		echo untouched > "$tmp/gpio/gpio$n/direction"
		echo untouched > "$tmp/gpio/gpio$n/value"
	done
	: > "$tmp/config.txt"
}
run() {
	TAB5_RAILS_GPIO="$tmp/gpio" TAB5_RAILS_PSY="$tmp/psy" \
		TAB5_RAILS_CFG="$tmp/config.txt" TAB5_RAILS_SETTLE=0 \
		sh "$tool" "$@" 2>&1
}
chg() { cat "$tmp/gpio/gpio527/direction"; }

# 1. boot, no keys: both 0x43 lines driven 0, the charger line is not
# touched -- the boot shim made that decision.
fresh
out=$(run); rc=$?
[ "$rc" -eq 0 ] && [ "$(cat "$tmp/gpio/gpio512/value")" = 0 ] &&
	[ "$(cat "$tmp/gpio/gpio514/value")" = 0 ] &&
	ok "boot, no keys: ext lines driven 0" || bad "boot nokeys: rc=$rc $out"
[ "$(chg)" = untouched ] &&
	echo "$out" | grep -qF 'charger=auto (the boot shim decided' &&
	ok "boot, charger unset: CHG_EN left as the boot shim set it" ||
	bad "boot auto: $(chg) / $out"

# 2. boot with the ext keys set
fresh
printf 'ext5v_en=1\next_ant_en = 1\r\n' > "$tmp/config.txt"
out=$(run); rc=$?
[ "$rc" -eq 0 ] && [ "$(cat "$tmp/gpio/gpio512/value")" = 1 ] &&
	[ "$(cat "$tmp/gpio/gpio514/value")" = 1 ] &&
	echo "$out" | grep -qF 'ext5v_en=1 applied (expander 0x43 P2' &&
	ok "boot: ext5v_en=1 / ext_ant_en=1 applied" || bad "boot ext: rc=$rc $out"

# 3. boot: charger=on and charger=off are forced, in one write each
fresh
echo 'charger=on' > "$tmp/config.txt"
out=$(run); rc=$?
[ "$rc" -eq 0 ] && [ "$(chg)" = high ] &&
	echo "$out" | grep -qF 'charger=on applied (expander 0x44 P7)' &&
	ok "boot: charger=on drives CHG_EN high" || bad "boot on: rc=$rc $(chg) $out"
[ "$(cat "$tmp/gpio/gpio527/value")" = untouched ] &&
	ok "the charger line is set through direction alone (no low glitch)" ||
	bad "charger line value was written"
fresh
echo 'charger=off' > "$tmp/config.txt"
out=$(run); rc=$?
[ "$rc" -eq 0 ] && [ "$(chg)" = low ] &&
	ok "boot: charger=off drives CHG_EN low" || bad "boot off: rc=$rc $(chg) $out"

# 4. live, auto: the pack test.  4 V or more on the node with the
# charger off is a pack.
fresh
echo 7912000 > "$tmp/psy/voltage_now"
out=$(run charger); rc=$?
[ "$rc" -eq 0 ] && [ "$(chg)" = high ] &&
	echo "$out" | grep -qF 'battery pack at 7912 mV; charger on' &&
	ok "live auto: a pack at 7.9 V turns the charger on" ||
	bad "live auto pack: rc=$rc $(chg) $out"
fresh
echo 1830000 > "$tmp/psy/voltage_now"
out=$(run charger); rc=$?
[ "$rc" -eq 0 ] && [ "$(chg)" = low ] &&
	echo "$out" | grep -qF 'no battery pack (1830 mV); charger off' &&
	ok "live auto: an empty node at 1.8 V leaves the charger off" ||
	bad "live auto nopack: rc=$rc $(chg) $out"
fresh
echo 3999000 > "$tmp/psy/voltage_now"
out=$(run charger)
[ "$(chg)" = low ] && ok "live auto: 3999 mV is no pack" || bad "3999: $(chg) $out"
echo 4000000 > "$tmp/psy/voltage_now"
out=$(run charger)
[ "$(chg)" = high ] && ok "live auto: 4000 mV is a pack" || bad "4000: $(chg) $out"
# unreadable voltage: the stock behaviour, charger on
fresh
out=$(run charger); rc=$?
[ "$rc" -eq 0 ] && [ "$(chg)" = high ] &&
	echo "$out" | grep -qF 'pack voltage not readable; charger on' &&
	ok "live auto: no voltage reading keeps the charger on" ||
	bad "live auto noread: rc=$rc $(chg) $out"
fresh
echo garbage > "$tmp/psy/voltage_now"
out=$(run charger)
[ "$(chg)" = high ] && ok "live auto: a non-numeric reading counts as unreadable" ||
	bad "garbage: $(chg) $out"

# 5. live, on/off: forced whatever the node reads; the ext lines are
# not touched on the charger path.
fresh
echo 'charger=on' > "$tmp/config.txt"
echo 1830000 > "$tmp/psy/voltage_now"
out=$(run charger); rc=$?
[ "$rc" -eq 0 ] && [ "$(chg)" = high ] &&
	ok "live: charger=on without a pack still turns it on" ||
	bad "live on: rc=$rc $(chg) $out"
[ "$(cat "$tmp/gpio/gpio512/value")" = untouched ] &&
	[ "$(cat "$tmp/gpio/gpio514/value")" = untouched ] &&
	ok "live charger path leaves the 0x43 lines alone" ||
	bad "live charger path touched the ext lines"
fresh
echo 'charger=off' > "$tmp/config.txt"
echo 7912000 > "$tmp/psy/voltage_now"
out=$(run charger)
[ "$(chg)" = low ] && ok "live: charger=off with a pack turns it off" ||
	bad "live off: $(chg) $out"

# 6. an invalid value is reported and treated as auto
fresh
echo 'charger=maybe' > "$tmp/config.txt"
out=$(run); rc=$?
echo "$out" | grep -qF 'ignoring invalid charger=maybe (use auto, on or off)' &&
	[ "$(chg)" = untouched ] &&
	ok "invalid charger value: reported, treated as auto" ||
	bad "invalid: rc=$rc $(chg) $out"

# 7. an unexported line is exported first
fresh
rm -rf "$tmp/gpio/gpio527"
echo 'charger=off' > "$tmp/config.txt"
out=$(run charger); rc=$?
[ "$(cat "$tmp/gpio/export")" = 527 ] &&
	ok "CHG_EN is line 7 of the 0x44 chip (gpio 527 here)" ||
	bad "export: $(cat "$tmp/gpio/export") rc=$rc $out"

# 8. a missing 0x44 chip fails the charger key only
fresh
rm -rf "$tmp/gpio/gpiochip520"
printf 'charger=on\next5v_en=1\n' > "$tmp/config.txt"
out=$(run); rc=$?
[ "$rc" -ne 0 ] && echo "$out" | grep -qF 'expander 0x44 gpiochip not found' &&
	[ "$(cat "$tmp/gpio/gpio514/value")" = 1 ] &&
	ok "no 0x44 chip: charger fails, the ext keys are still applied" ||
	bad "no 0x44: rc=$rc $out"
fresh
rm -rf "$tmp/gpio/gpiochip512"
out=$(run); rc=$?
[ "$rc" -ne 0 ] && echo "$out" | grep -qF 'expander 0x43 gpiochip not found' &&
	ok "no 0x43 chip: reported as before" || bad "no 0x43: rc=$rc $out"

# 9. usage
fresh
out=$(run bogus); rc=$?
[ "$rc" -eq 2 ] && ok "unknown argument: usage, rc 2" || bad "usage: rc=$rc $out"

# 10. the boot shim's half of the contract, in its source: the charger
# is not enabled in the expander init any more, the decision is made
# after the C6 reset wait from the pack node voltage, and a failed read
# keeps the stock behaviour.
! grep -q 'EXP1_OUT_BOOT' "$shim" &&
	ok "shim: no unconditional charge-enable value left" ||
	bad "shim still has EXP1_OUT_BOOT"
[ "$(grep -c 'EXP1_OUT_CHARGE' "$shim")" = 2 ] &&
	ok "shim: the charge-enable value is written in one place" ||
	bad "shim: EXP1_OUT_CHARGE used $(grep -c 'EXP1_OUT_CHARGE' "$shim") times"
awk '/^static esp_err_t expanders_init\(void\)/ { f = 1 }
     f && /EXP1_OUT_CHARGE/ { bad = 1 }
     f && /^}/ { exit }
     END { exit bad }' "$shim" &&
	ok "shim: expanders_init leaves the charger off" ||
	bad "shim: expanders_init enables the charger"
awk '/pulse_c6_reset\(\);/ { p = NR }
     /^\t\tcharger_decide\(\);/ { c = NR }
     END { exit !(p && c && c > p && c - p <= 3) }' "$shim" &&
	ok "shim: charger_decide() runs right after the C6 reset wait" ||
	bad "shim: charger_decide() is not called after pulse_c6_reset()"
grep -q '#define TAB5_PACK_PRESENT_MV 4000' "$shim" &&
	grep -q 'mv >= 0 && mv < TAB5_PACK_PRESENT_MV' "$shim" &&
	ok "shim: below 4000 mV is no pack; an unreadable node is not" ||
	bad "shim: pack threshold / unreadable handling changed"
grep -q '#define C6_RESET_READY_DELAY_US 1500000u' "$shim" &&
	ok "shim: the node has 1.5 s to fall before it is read" ||
	bad "shim: C6 reset wait changed -- the pack test relies on it"

if [ "$fails" -gt 0 ]; then
	echo "FAILED: $fails checks"
	exit 1
fi
echo "PASS: tab5-rails ext keys and the charger key"
