#!/bin/sh
# S40network stop_wpa sends wpa_supplicant ONE SIGTERM and waits.  On its way
# out wpa_supplicant switches a SoftAP back to station mode; a second signal
# during that command cut the esp-hosted driver's wait short, and a live
# switch from SoftAP to station then left wlan0 without a MAC address
# ("ifconfig wlan0 up": Cannot assign requested address).  Only a daemon
# still alive after 10 s gets the every-process TERM.
set -u

repo_root=$(cd -- "$(dirname -- "$0")/../.." && pwd)
script="$repo_root/linux/m3-lab/rootfs-overlay/etc/init.d/S40network"
tmp=${TMPDIR:-/tmp}/wpa-stop-test.$$
trap 'rm -rf -- "$tmp"' EXIT HUP INT TERM
mkdir -p "$tmp"

sed -n '/^stop_sup()/,/^}/p;/^stop_wpa()/,/^}/p;/^wpa_alive()/,/^}/p' \
	"$script" > "$tmp/funcs.sh"

WPA_SUP=$tmp/sup
WPA_PID=$tmp/pid

# Stubs.  The supervisor is pid 100, wpa_supplicant pid 200; the daemon
# stays in the process list for $exit_after ticks after the first signal.
log() { echo "$*" >> "$tmp/log"; }
kill() {
	log "kill $*"
	case "$*" in
		"-0 200"|200) [ "$wpa_up" -eq 1 ] ;;
		100) [ "$sup_up" -eq 1 ] ;;
		*) return 1 ;;
	esac
}
sig_if_running() { log "sig $1 $2"; }
proc_running() { [ "$1" = wpa_supplicant ] && [ "$ticks" -lt "$exit_after" ]; }
net_tick() { ticks=$((ticks + 1)); }

. "$tmp/funcs.sh"

fails=0
check() { # name expected-log
	got=$(cat "$tmp/log" 2>/dev/null | tr '\n' ';')
	if [ "$got" = "$2" ] && [ ! -e "$WPA_SUP" ] && [ ! -e "$WPA_PID" ]; then
		echo "ok: $1"
	else
		echo "FAIL: $1: got '$got', want '$2'" \
			"(sup file $([ -e "$WPA_SUP" ] && echo left || echo gone)," \
			"pid file $([ -e "$WPA_PID" ] && echo left || echo gone))"
		fails=$((fails + 1))
	fi
}
run() { # sup-file pid-file sup_up wpa_up exit_after
	rm -f "$tmp/log" "$WPA_SUP" "$WPA_PID"
	[ -n "$1" ] && echo "$1" > "$WPA_SUP"
	[ -n "$2" ] && echo "$2" > "$WPA_PID"
	sup_up=$3 wpa_up=$4 exit_after=$5 ticks=0
	stop_wpa
}

run 100 200 1 1 2
check "a supervised daemon gets one TERM, from its supervisor" "kill 100;"
[ "$ticks" -eq 2 ] || { echo "FAIL: waited $ticks ticks, want 2"; fails=$((fails + 1)); }

run 100 200 0 1 2
check "a dead supervisor: the daemon itself gets one TERM" "kill 100;kill -0 200;kill 200;"

run "" "" 0 0 2
check "no pid files: one TERM to every wpa_supplicant" "sig wpa_supplicant TERM;"

run 100 200 1 1 99
check "a daemon still alive after 10 s gets the second TERM" "kill 100;sig wpa_supplicant TERM;"
[ "$ticks" -eq 15 ] || { echo "FAIL: stuck daemon waited $ticks ticks, want 15"; fails=$((fails + 1)); }

run 100 200 1 1 0
check "a daemon already gone: no second TERM, no wait" "kill 100;"
[ "$ticks" -eq 0 ] || { echo "FAIL: waited $ticks ticks for a gone daemon"; fails=$((fails + 1)); }

if [ "$fails" -eq 0 ]; then
	echo "PASS: S40network stops wpa_supplicant with one TERM"
else
	echo "FAIL: $fails checks failed" >&2
	exit 1
fi
