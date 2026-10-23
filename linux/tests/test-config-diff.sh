#!/bin/sh
# Host-side check of bootsync's config.txt key diff (#39): extracts
# cfg_norm and net_changed_keys from the real script and runs them under
# plain sh against old/new fixture pairs.
set -u

repo_root=$(cd -- "$(dirname -- "$0")/../.." && pwd)
bootsync="$repo_root/linux/m3-lab/rootfs-overlay/usr/sbin/bootsyncd"
work=${TMPDIR:-/tmp}/cfgdiff-test.$$
trap 'rm -rf -- "$work"' EXIT HUP INT TERM
mkdir -p "$work"

# Source just the two functions; their /run temp paths move under $work.
RUNDIR=$work
export RUNDIR
sed -n '/^cfg_norm()/,/^}/p;/^net_changed_keys()/,/^}/p' "$bootsync" |
	sed "s|/run/|\$RUNDIR/|g" > "$work/funcs.sh"
. "$work/funcs.sh"

fails=0
check() { # name, expected CHANGED_KEYS ("" = no change)
	net_changed_keys "$work/new.txt" "$work/old.txt"
	got=$CHANGED_KEYS
	if [ "$got" = "$2" ]; then
		echo "ok: $1"
	else
		echo "FAIL: $1: expected '$2' got '$got'"
		fails=$((fails + 1))
	fi
}

cat > "$work/old.txt" <<'EOF'
hostname=stamp-p4
wifi_ssid=Net
wifi_psk=secret123
ssh_password_auth=no
autorun=main.py
EOF

cp "$work/old.txt" "$work/new.txt"
check "identical" ""

printf 'hostname=stamp-p4\nwifi_ssid=Net\nwifi_psk=secret123\nssh_password_auth=no\nautorun=main2.py\n' > "$work/new.txt"
check "autorun-only change" ""

printf 'hostname=stamp-p4\nwifi_ssid=Net\nwifi_psk=secret123\nssh_password_auth=yes\nautorun=main.py\n' > "$work/new.txt"
check "boot-only key" " ssh_password_auth"

printf '\xef\xbb\xbf# comment\r\nhostname = stamp-p4\r\nwifi_ssid = Other\nwifi_psk=secret123\n\nssh_password_auth=no\nautorun=main.py\n' > "$work/new.txt"
check "BOM+CRLF+comment+spaces ssid change" " wifi_ssid"

# conf_get keeps trailing blanks in the value, so "stamp-p4 " counts as a
# change; the diff is faithful to that.
printf 'hostname=stamp-p4 \nwifi_ssid=Net\nwifi_psk=secret123\nssh_password_auth=no\nautorun=main.py\n' > "$work/new.txt"
check "trailing blank is a value change" " hostname"
printf 'hostname=stamp-p4\nwifi_ssid=Net\nwifi_psk=secret123\nssh_password_auth=no\nautorun=main.py\n' > "$work/new.txt"

printf 'hostname=stamp-p4\nwifi_ssid=Net\nwifi_psk=secret123\nssh_password_auth=no\nautorun=main.py\nwifi_mode=ap\n' > "$work/new.txt"
check "added wifi_mode" " wifi_mode"

printf 'hostname=first\nhostname=second\nwifi_ssid=Net\n' > "$work/old.txt"
printf 'hostname=first\nwifi_ssid=Net\n' > "$work/new.txt"
check "duplicate key, first wins" ""

printf 'hostname=first\nwifi_ssid=Net\n' > "$work/old.txt"
# The second occurrence is ignored by conf_get's first-wins read, so the
# effective value did not change.
printf 'hostname=first\nhostname=second\nwifi_ssid=Net\n' > "$work/new.txt"
check "duplicate added after first" ""

rm -f "$work/old.txt"
net_changed_keys "$work/new.txt" "$work/old.txt"
if [ "$CHANGED_KEYS" = full ]; then
	echo "ok: missing baseline -> full"
else
	echo "FAIL: missing baseline: expected 'full' got '$CHANGED_KEYS'"
	fails=$((fails + 1))
fi

[ "$fails" -eq 0 ] && echo "PASS: config.txt changed-key detection" || exit 1
