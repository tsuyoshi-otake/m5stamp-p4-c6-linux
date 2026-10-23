# Sourced by S05tmpfs at boot and by bootsyncd when the USB
# drive changes.  The caller defines es_log.
#
# Fail-safe contract (#40): a function that cannot finish -- including an
# exec failing with ENOMEM -- returns 1, leaves either the old or the new
# state (never a half-written file), and logs ES_*_FAIL.  bootsync then
# keeps the old file in /tmp so the next save retries.  The reads and the
# shadow edit below use only shell builtins, so they cannot fail on exec.

# Apply $3= from credentials.txt ($1) to user $2.  /etc is read-only squashfs,
# so a patched copy of /etc/shadow on /run is bind-mounted over it.  Once it is
# mounted, a new password overwrites that copy in place.  $4 is the short
# failure label used in the log ("m5 password login stays disabled" /
# "root login stays locked").
es_apply_user_password() {
	[ -f "$1" ] || return 0
	cr=$(printf '\r')
	# First "key=" wins, same order the old sed -n ... | head -n1 read.
	pw=""
	while IFS= read -r l || [ -n "$l" ]; do
		l=${l%"$cr"}
		case "$l" in
			"$3="*) pw=${l#*=}; break ;;
		esac
	done < "$1"
	case "$pw" in
		''|*:*) es_log "ES_CREDENTIALS: no usable $3= in credentials.txt"; return 0 ;;
	esac
	if [ "${#pw}" -lt 8 ] || [ "${#pw}" -gt 64 ]; then
		es_log "ES_CREDENTIALS: $3= must be 8-64 characters; $4"
		return 0
	fi
	hash=$(printf '%s\n' "$pw" | cryptpw -m sha512 2>/dev/null) || hash=""
	case "$hash" in
		'$6$'*) ;;
		*) es_log "ES_CREDENTIALS FAIL: cryptpw unavailable; $4"; return 1 ;;
	esac
	# Rewrite the user's field 2 with builtins; a partial write lands in
	# shadow.new only, never in the live file.  /run/shadow holds the
	# last-applied state and is the same inode the bind mount shows; read
	# it so a retry after an unbound bind still merges the right file.
	src=/etc/shadow
	[ -f /run/shadow ] && src=/run/shadow
	{
		while IFS= read -r l || [ -n "$l" ]; do
			case "$l" in
				"$2:"*) printf '%s\n' "$2:$hash:${l#*:*:}" ;;
				*) printf '%s\n' "$l" ;;
			esac
		done < "$src"
	} > /run/shadow.new
	if ! chmod 600 /run/shadow.new 2>/dev/null; then
		rm -f /run/shadow.new
		es_log "ES_CREDENTIALS FAIL: could not apply the password; $4"
		return 1
	fi
	if grep -q ' /etc/shadow ' /proc/mounts; then
		# /etc/shadow is a bind mount: a rename cannot swap the mounted
		# inode, so drop the bind, rename, and rebind.  In between, the
		# squashfs shadow (root locked, m5 disabled) shows through --
		# a safe state -- and there is never a truncated live file.
		if umount /etc/shadow 2>/dev/null; then
			if mv -f /run/shadow.new /run/shadow 2>/dev/null &&
				mount --bind /run/shadow /etc/shadow 2>/dev/null; then
				es_log "ES_CREDENTIALS: $2 password applied from credentials.txt"
				return 0
			fi
			# Rebind whatever /run/shadow holds so the next retry sees
			# a bind mount again.
			mount --bind /run/shadow /etc/shadow 2>/dev/null || true
		elif {
			# The mount is busy (a login has shadow open): write the new
			# content through the bind -- a few hundred bytes in a single
			# builtin printf burst.
			while IFS= read -r l || [ -n "$l" ]; do
				printf '%s\n' "$l"
			done < /run/shadow.new
		} > /run/shadow 2>/dev/null; then
			rm -f /run/shadow.new
			es_log "ES_CREDENTIALS: $2 password applied from credentials.txt"
			return 0
		fi
	elif mv -f /run/shadow.new /run/shadow &&
		mount --bind /run/shadow /etc/shadow; then
		es_log "ES_CREDENTIALS: $2 password applied from credentials.txt"
		return 0
	fi
	rm -f /run/shadow.new
	es_log "ES_CREDENTIALS FAIL: could not apply the password; $4"
	return 1
}

apply_password() {
	es_apply_user_password "$1" m5 password "m5 password login stays disabled"
}

# root_password= from credentials.txt unlocks su (#37).  Without it the
# image's root:! lock (post-build.sh) stays and su keeps failing.
apply_root_password() {
	es_apply_user_password "$1" root root_password "root login stays locked"
}

# Install the keys of drive file $1 as $3/.ssh/authorized_keys for user $2
# (owner $4); $5 is the drive file name for the log.  /home and /root are on
# the /tmp ramdisk, which can be full: the new file is built next to the old
# one and renamed over it, and a failed build removes the keys rather than
# leaving a partial file, so keys taken out of the drive file never stay
# valid.
# A printf that hits ENOSPC inside the loop does not fail the loop, so read
# the file back: it must hold $2 complete lines (a cut-off last line has no
# newline).  Builtins only.
es_keys_complete() {
	c=0
	while IFS= read -r l; do
		c=$((c + 1))
	done < "$1"
	[ -z "$l" ] && [ "$c" -eq "$2" ]
}

es_install_keys() {
	[ -f "$1" ] || return 0
	d=$3/.ssh
	tmp="$d/.authorized_keys.new"
	cr=$(printf '\r')
	n=0
	# Strip CRs and skip comment/blank lines with builtins; leading blanks
	# are trimmed for the test only and kept in the written line, as the
	# old sed|grep|grep chain did.
	if ! mkdir -p "$d" 2>/dev/null ||
		! {
			while IFS= read -r l || [ -n "$l" ]; do
				l=${l%"$cr"}
				t=$l
				while :; do
					case "$t" in
						" "*) t=${t#?} ;;
						"	"*) t=${t#?} ;;
						*) break ;;
					esac
				done
				case "$t" in ''|'#'*) continue ;; esac
				printf '%s\n' "$l"
				n=$((n + 1))
			done < "$1"
		} > "$tmp" 2>/dev/null || ! es_keys_complete "$tmp" "$n"; then
		rm -f "$tmp" "$d/authorized_keys"
		es_log "ES_SSH_KEYS FAIL: cannot write $d/authorized_keys (is /tmp full?); no keys for $2 until the next save or boot"
		return 1
	fi
	if ! mv -f "$tmp" "$d/authorized_keys" 2>/dev/null; then
		rm -f "$tmp" "$d/authorized_keys"
		es_log "ES_SSH_KEYS FAIL: cannot write $d/authorized_keys (is /tmp full?); no keys for $2 until the next save or boot"
		return 1
	fi
	chown -R "$4" "$d" 2>/dev/null || true
	chmod 700 "$d"
	chmod 600 "$d/authorized_keys"
	: > "/run/.es-keys-$2"
	es_log "ES_SSH_KEYS: $n key(s) installed for $2 from $5"
	return 0
}

# authorized_keys on the USB drive -> /home/m5/.ssh (#25).  /home is a tmpfs
# overlay, so this runs every boot and the drive stays the source of truth.
install_keys() {
	es_install_keys "$1" m5 /home/m5 1000:1000 authorized_keys
}

# root_authorized_keys on the USB drive -> /root/.ssh (#37).  Same mechanism
# as install_keys; Dropbear's -g keeps root to keys only.
install_root_keys() {
	es_install_keys "$1" root /root 0:0 root_authorized_keys
}

# A key file deleted from the drive revokes the keys it installed at once
# (#37), as deleting authorized_keys does elsewhere.  The /run marker keeps
# keys added by hand over SSH when the drive never had the file.
# $1 user, $2 home, $3 drive file name.
revoke_keys() {
	[ -e "/run/.es-keys-$1" ] || return 0
	rm -f "$2/.ssh/authorized_keys" "/run/.es-keys-$1"
	es_log "ES_SSH_KEYS: $3 removed from the drive; keys for $1 revoked"
}
