# Sourced by S05easystick-tmpfs at boot and by easystick-bootsync when the USB
# drive changes.  The caller defines es_log.

# Apply $3= from credentials.txt ($1) to user $2.  /etc is read-only squashfs,
# so a patched copy of /etc/shadow on /run is bind-mounted over it.  Once it is
# mounted, a new password overwrites that copy in place.  $4 is the short
# failure label used in the log ("m5 password login stays disabled" /
# "root login stays locked").
es_apply_user_password() {
	[ -f "$1" ] || return 0
	cr=$(printf '\r')
	pw=$(sed -e "s/$cr\$//" "$1" | sed -n "s/^$3=//p" | head -n1)
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
		*) es_log "ES_CREDENTIALS FAIL: cryptpw unavailable; $4"; return 0 ;;
	esac
	if ! sed "s|^$2:[^:]*:|$2:${hash}:|" /etc/shadow > /run/shadow.new ||
		! chmod 600 /run/shadow.new; then
		rm -f /run/shadow.new
		es_log "ES_CREDENTIALS FAIL: could not apply the password; $4"
		return 0
	fi
	if grep -q ' /etc/shadow ' /proc/mounts; then
		cat /run/shadow.new > /run/shadow && rm -f /run/shadow.new &&
			es_log "ES_CREDENTIALS: $2 password applied from credentials.txt" && return 0
	elif mv -f /run/shadow.new /run/shadow && mount --bind /run/shadow /etc/shadow; then
		es_log "ES_CREDENTIALS: $2 password applied from credentials.txt"
		return 0
	fi
	rm -f /run/shadow.new
	es_log "ES_CREDENTIALS FAIL: could not apply the password; $4"
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
# the /tmp ramdisk, which can be full: the copy is checked against the size
# of the keys, and a short one is removed rather than left half-written, so
# keys taken out of the drive file never stay valid.
es_install_keys() {
	[ -f "$1" ] || return 0
	d=$3/.ssh
	new=/run/.es-keys-new.$$
	cr=$(printf '\r')
	sed -e "s/$cr\$//" "$1" | grep -v '^[[:space:]]*#' | grep -v '^[[:space:]]*$' > "$new" || true
	n=$(grep -c '' "$new" || true)
	if mkdir -p "$d" 2>/dev/null && cat "$new" > "$d/authorized_keys" 2>/dev/null &&
	   [ "$(wc -c < "$d/authorized_keys")" -eq "$(wc -c < "$new")" ]; then
		chown -R "$4" "$d" 2>/dev/null || true
		chmod 700 "$d"
		chmod 600 "$d/authorized_keys"
		: > "/run/.es-keys-$2"
		es_log "ES_SSH_KEYS: $n key(s) installed for $2 from $5"
	else
		rm -f "$d/authorized_keys"
		es_log "ES_SSH_KEYS FAIL: cannot write $d/authorized_keys (is /tmp full?); no keys for $2 until the next save or boot"
	fi
	rm -f "$new"
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
