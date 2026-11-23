#!/bin/sh
# bootsyncd C6 link watch (#41): two misses in a row on the driver's
# observation node ask S40network for a re-init; the watch then holds off,
# slows down when the re-inits do not help, and leaves a healthy link, a
# missing driver and a reload in progress alone.
set -u

repo_root=$(cd -- "$(dirname -- "$0")/../.." && pwd)
daemon="$repo_root/linux/m3-lab/rootfs-overlay/usr/sbin/bootsyncd"
tmp=${TMPDIR:-/tmp}/bootsyncd-c6watch-test.$$
trap 'rm -rf -- "$tmp"' EXIT HUP INT TERM
mkdir -p "$tmp"

# Source just the two watches; their globals are set by reset() below.
sed -n '/^c6_watch()/,/^}/p; /^ap_watch()/,/^}/p' "$daemon" > "$tmp/funcs.sh"

# The S40network stand-in records how it was called.
cat > "$tmp/net-ctl" <<EOF
#!/bin/sh
echo "\$*" >> "$tmp/calls"
EOF
chmod +x "$tmp/net-ctl"

log() { echo "$1" >> "$tmp/log"; }

C6_OBS="$tmp/obs"
C6_NET_CTL="$tmp/net-ctl"
C6_NET_LOCK="$tmp/lock"
C6_STATUS="$tmp/status"
reset() {
	C6_WATCH_TICKS=10
	C6_TICKS=0
	C6_MISS=0
	C6_HOLD=0
	C6_TRIES=0
	AP_MISS=0
	AP_TRIES=0
	rm -rf "$tmp/calls" "$tmp/log" "$tmp/lock" "$C6_OBS" "$C6_STATUS"
}
# status STATE: a status file in update-status's shape
status() {
	printf '# Network Status\nstatus=%s\nwifi_mode=ap\nip_address=none\n' "$1" > "$C6_STATUS"
}
ticks() { # n: n one-second ticks, then let the requests finish
	i=0
	while [ "$i" -lt "$1" ]; do
		c6_watch
		i=$((i + 1))
	done
	wait
}
calls() {
	c=0
	[ -f "$tmp/calls" ] && c=$(grep -c . "$tmp/calls")
	echo "$c"
}

. "$tmp/funcs.sh"

fails=0
want_calls() { # name n
	got=$(calls)
	if [ "$got" = "$2" ]; then
		echo "ok: $1"
	else
		echo "FAIL: $1: expected $2 request(s), got $got"
		fails=$((fails + 1))
	fi
}

# 1. No observation node: the driver is not loaded, nothing is asked.
reset
ticks 100
want_calls "no node, no request" 0

# 2. A link that answers is left alone.
reset
echo "status=ok" > "$C6_OBS"
ticks 100
want_calls "a healthy link, no request" 0

# 3. A dead link: nothing after the first miss (10 s), the request after
# the second (20 s), and it is the c6link apply.
reset
echo "status=read_fail ret=-110" > "$C6_OBS"
ticks 19
want_calls "one miss is not enough" 0
ticks 1
want_calls "two misses in a row ask for the re-init" 1
[ "$(cat "$tmp/calls")" = "apply c6link" ] &&
	echo "ok: the request is 'apply c6link'" ||
	{ echo "FAIL: request was '$(cat "$tmp/calls")'"; fails=$((fails + 1)); }
grep -q 'C6 link is dead (status=read_fail ret=-110)' "$tmp/log" &&
	echo "ok: the log says what the node answered" ||
	{ echo "FAIL: log: $(cat "$tmp/log")"; fails=$((fails + 1)); }

# 4. Hold-off: nine checks (90 s) pass without a look at the node, then two
# more misses are needed -- the second request comes 110 s after the first.
ticks 109
want_calls "no second request during the hold-off" 1
ticks 1
want_calls "the second request after the hold-off and two misses" 2

# 5. A single miss between good answers is a hiccup, not a death.
reset
echo "status=read_fail ret=-110" > "$C6_OBS"
ticks 10
echo "status=ok" > "$C6_OBS"
ticks 10
echo "status=read_fail ret=-110" > "$C6_OBS"
ticks 10
want_calls "misses that are not in a row ask for nothing" 0

# 6. A reload in progress (S40network's lock) owns the link.
reset
echo "status=no_func" > "$C6_OBS"
mkdir "$tmp/lock"
ticks 100
want_calls "no request while a reload holds the lock" 0
rmdir "$tmp/lock"
ticks 20
want_calls "the watch goes on once the lock is gone" 1

# 7. Re-inits that do not help: three at the short interval, then one in
# ten minutes, said once.
reset
echo "status=read_fail ret=-110" > "$C6_OBS"
ticks 20
ticks 110
ticks 110
want_calls "three re-inits at the short interval" 3
ticks 110
want_calls "the fourth still follows the short hold-off" 4
[ "$(grep -c 'one attempt in ten minutes' "$tmp/log")" = 1 ] &&
	echo "ok: the slow-down is announced" ||
	{ echo "FAIL: slow-down message: $(cat "$tmp/log")"; fails=$((fails + 1)); }
ticks 609
want_calls "no request during the ten minutes" 4
ticks 1
want_calls "one more request after the ten minutes" 5
[ "$(grep -c 'one attempt in ten minutes' "$tmp/log")" = 1 ] &&
	echo "ok: the slow-down is announced once" ||
	{ echo "FAIL: slow-down message repeated"; fails=$((fails + 1)); }

# 8. The link is back: said once, and the count starts again.
ticks 600
echo "status=ok" > "$C6_OBS"
ticks 10
[ "$(grep -c 'C6 link answers again' "$tmp/log")" = 1 ] && [ "$C6_TRIES" = 0 ] &&
	echo "ok: a returned link resets the watch" ||
	{ echo "FAIL: return of the link: tries=$C6_TRIES"; fails=$((fails + 1)); }
ticks 100
[ "$(grep -c 'C6 link answers again' "$tmp/log")" = 1 ] &&
	echo "ok: a healthy link is not announced again" ||
	{ echo "FAIL: 'answers again' repeated"; fails=$((fails + 1)); }

# 9. The daemon's loop runs the watch every tick, before the drive states
# that "continue" past the rest of the loop body.
sed -n '/^while true; do$/,/^done$/p' "$daemon" | sed -n '2,4p' | grep -q '^	c6_watch$' &&
	echo "ok: the main loop runs the watch every tick" ||
	{ echo "FAIL: c6_watch is not at the top of the main loop"; fails=$((fails + 1)); }

# 10. The watches exec nothing while the link is fine (NOMMU: an exec is
# ~0.5 MB): no command substitution, no external program.
if sed -n '/^c6_watch()/,/^}/p; /^ap_watch()/,/^}/p' "$daemon" | grep -v 'C6_NET_CTL' | grep -qE '\$\([^(]|`|grep|sed|cat '; then
	echo "FAIL: c6_watch/ap_watch run an external program on the healthy path"
	fails=$((fails + 1))
else
	echo "ok: the watches use builtins only"
fi

# 11. The SoftAP watch: a link that answers with the SoftAP up, a station
# result or a stopped network asks for nothing.
for st in AP_ACTIVE DHCP_FAILED AUTH_FAILED STOPPED; do
	reset
	echo "status=ok" > "$C6_OBS"
	status "$st"
	ticks 100
	want_calls "link ok, status=$st: no request" 0
done

# 12. A link that answers but status=AP_FAILED (the C6 came back without
# its access point): nothing after the first look, a restart after the
# second, said in the log.
reset
echo "status=ok" > "$C6_OBS"
status AP_FAILED
ticks 19
want_calls "one look at AP_FAILED is not enough" 0
ticks 1
want_calls "two looks at AP_FAILED ask for a restart" 1
[ "$(cat "$tmp/calls")" = "restart" ] &&
	echo "ok: the request is 'restart'" ||
	{ echo "FAIL: request was '$(cat "$tmp/calls")'"; fails=$((fails + 1)); }
grep -q 'SoftAP did not start (status=AP_FAILED)' "$tmp/log" &&
	echo "ok: the log says why" ||
	{ echo "FAIL: log: $(cat "$tmp/log")"; fails=$((fails + 1)); }

# 13. The same hold-off and slow-down as the link watch: three restarts
# 110 s apart, then one in ten minutes, announced once.
ticks 109
want_calls "no second restart during the hold-off" 1
ticks 1
want_calls "the second restart after the hold-off and two looks" 2
ticks 110
ticks 110
want_calls "the fourth still follows the short hold-off" 4
[ "$(grep -c 'three restarts did not help' "$tmp/log")" = 1 ] &&
	echo "ok: the SoftAP slow-down is announced" ||
	{ echo "FAIL: SoftAP slow-down message: $(cat "$tmp/log")"; fails=$((fails + 1)); }
ticks 609
want_calls "no restart during the ten minutes" 4
ticks 1
want_calls "one more restart after the ten minutes" 5

# 14. The SoftAP is back: said once, and the count starts again.
ticks 600
status AP_ACTIVE
ticks 10
[ "$(grep -c 'network status is AP_ACTIVE now' "$tmp/log")" = 1 ] && [ "$AP_TRIES" = 0 ] &&
	echo "ok: a SoftAP that is up again resets the watch" ||
	{ echo "FAIL: SoftAP back: tries=$AP_TRIES $(cat "$tmp/log")"; fails=$((fails + 1)); }
ticks 100
[ "$(grep -c 'network status is' "$tmp/log")" = 1 ] &&
	echo "ok: a SoftAP that stays up is not announced again" ||
	{ echo "FAIL: 'network status' repeated"; fails=$((fails + 1)); }

# 15. A restart in progress holds S40network's lock: no second request.
reset
echo "status=ok" > "$C6_OBS"
status AP_FAILED
mkdir "$tmp/lock"
ticks 100
want_calls "no restart while the lock is held" 0
rmdir "$tmp/lock"
ticks 20
want_calls "the SoftAP watch goes on once the lock is gone" 1

# 16. A dead link is the link watch's: the c6link re-init, never a
# restart, whatever the status file still says.
reset
echo "status=read_fail ret=-110" > "$C6_OBS"
status AP_FAILED
ticks 20
[ "$(cat "$tmp/calls" 2>/dev/null)" = "apply c6link" ] &&
	echo "ok: a dead link gets the re-init, not a restart" ||
	{ echo "FAIL: dead link + AP_FAILED: $(cat "$tmp/calls" 2>/dev/null)"; fails=$((fails + 1)); }

# 17. After a re-init that left the SoftAP down, the link answers again:
# the SoftAP watch takes over and asks for the restart.
echo "status=ok" > "$C6_OBS"
ticks 110
[ "$(grep -c . "$tmp/calls")" = 2 ] && [ "$(sed -n 2p "$tmp/calls")" = "restart" ] &&
	echo "ok: re-init then AP_FAILED: the restart follows" ||
	{ echo "FAIL: re-init then AP_FAILED: $(cat "$tmp/calls")"; fails=$((fails + 1)); }

# 18. No status file (S40network has not run yet): nothing.
reset
echo "status=ok" > "$C6_OBS"
ticks 100
want_calls "no status file, no request" 0

if [ "$fails" -eq 0 ]; then
	echo "PASS: bootsyncd C6 link watch"
else
	echo "FAIL: $fails checks failed" >&2
	exit 1
fi
