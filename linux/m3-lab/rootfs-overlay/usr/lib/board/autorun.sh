# Pico-style autorun (#34): main.py, main.sh or main (a NOMMU bFLT binary) on
# the USB drive runs as m5.  Sourced by S05tmpfs (stage at boot),
# bootsyncd (start, and restart when the drive's program changes)
# and autorun (the command).  The caller defines es_log.
#
# config.txt: autorun=off turns it off, autorun=NAME runs NAME instead of the
# first of main.py, main.sh, main; autorun_restart=yes restarts the program
# 5 s after it exits.  Without it a program that exits stays stopped, as on a
# Pico.
#
# The program and its *.py neighbours and lib/ are copied to /run/autorun
# (owned by m5, the working directory).  Output goes to /run/autorun.log,
# which bootsync trims to AUTORUN_LOG_MAX bytes with one old copy.  Nothing is
# written back to the drive: the host owns it.

AUTORUN_DIR=/run/autorun
AUTORUN_NEW=/run/.autorun-new
AUTORUN_SUM=/run/.autorun-md5
AUTORUN_PID=/run/autorun.pid
AUTORUN_LOG=/run/autorun.log
AUTORUN_LOG_REF=/run/.autorun-log-seen
AUTORUN_LOG_MAX=65536
AUTORUN_USER=m5
# Written into the copy, so a change of the settings also counts as a change.
AUTORUN_SETTINGS=.autorun

# Only root can write /run.  A builtin test: $(id -u) would cost a shell copy.
autorun_as_root() {
	[ -w /run ]
}

# AUTORUN_MODE (auto, off or a file name) and AUTORUN_RESTART (0/1) from
# config.txt $1.  A name keeps only [A-Za-z0-9._-]: that drops a CR and
# trailing comments, and the result cannot leave the program directory.
autorun_config() {
	AUTORUN_MODE=auto
	AUTORUN_RESTART=0
	[ -f "$1" ] || return 0
	while IFS= read -r l || [ -n "$l" ]; do
		case "$l" in
			autorun=*)
				v=${l#autorun=}
				v=${v%%[!A-Za-z0-9._-]*}
				case "$v" in
					''|off|no|0) AUTORUN_MODE=off ;;
					auto|yes|1) AUTORUN_MODE=auto ;;
					.*) ;;
					*) AUTORUN_MODE=$v ;;
				esac
				;;
			autorun_restart=yes|autorun_restart=yes[!A-Za-z0-9_]*) AUTORUN_RESTART=1 ;;
			autorun_restart=*) AUTORUN_RESTART=0 ;;
		esac
	done < "$1"
}

# Copy the program files from the drive mounted at $1 to AUTORUN_NEW.
autorun_stage() {
	rm -rf "$AUTORUN_NEW"
	mkdir -p "$AUTORUN_NEW" || return 1
	autorun_config "$1/config.txt"
	echo "mode=$AUTORUN_MODE restart=$AUTORUN_RESTART" > "$AUTORUN_NEW/$AUTORUN_SETTINGS" || return 1
	if [ "$AUTORUN_MODE" != off ]; then
		for f in "$1"/*.py "$1/main.sh" "$1/main" "$1/$AUTORUN_MODE"; do
			[ -f "$f" ] || continue
			cp -f "$f" "$AUTORUN_NEW/" || return 1
		done
		if [ -d "$1/lib" ]; then
			cp -r "$1/lib" "$AUTORUN_NEW/lib" || return 1
		fi
		# Editors on Windows save CRLF, which sh reads as part of each word.
		# Unquoted: hush escapes a quoted "-" as "\-" and uClibc glob() does
		# not unescape a directory part, so "$AUTORUN_NEW" (it has a "-")
		# followed by a glob never expands.
		for f in $AUTORUN_NEW/*.sh; do
			[ -f "$f" ] || continue
			tr -d '\r' < "$f" > "$f.lf" && mv -f "$f.lf" "$f"
		done
	fi
	# The log and PID file are m5's, so autorun works for m5 too.
	for f in "$AUTORUN_LOG" "$AUTORUN_PID"; do
		[ -f "$f" ] && continue
		: > "$f" && chown "$AUTORUN_USER:$AUTORUN_USER" "$f" && chmod 644 "$f"
	done
	chown -R "$AUTORUN_USER:$AUTORUN_USER" "$AUTORUN_NEW" &&
		chmod -R u+rwX,go+rX "$AUTORUN_NEW"
}

# Liveness without exec: a zombie counts as gone.
autorun_alive() {
	[ -n "$1" ] && [ -r "/proc/$1/stat" ] || return 1
	st=""
	read -r _ _ st _ 2>/dev/null < "/proc/$1/stat" || return 1
	[ -n "$st" ] && [ "$st" != Z ]
}

autorun_running() {
	AUTORUN_RUNNING_PID=""
	[ -f "$AUTORUN_PID" ] || return 1
	read -r AUTORUN_RUNNING_PID 2>/dev/null < "$AUTORUN_PID" || true
	case "$AUTORUN_RUNNING_PID" in
		''|*[!0-9]*) AUTORUN_RUNNING_PID=""; return 1 ;;
	esac
	autorun_alive "$AUTORUN_RUNNING_PID"
}

# The program leads its own session (chpst -P), so its children go with it.
# The PID file is m5's, so root sends the signals as m5 and cannot be pointed
# at another user's process.
autorun_signal() {
	if autorun_as_root; then
		chpst -u "$AUTORUN_USER" kill "-$1" "-$2" "$2" 2>/dev/null
	else
		kill "-$1" "-$2" "$2" 2>/dev/null
	fi
}

autorun_stop() {
	autorun_running || return 0
	pid=$AUTORUN_RUNNING_PID
	autorun_signal TERM "$pid"
	# bootsync's FIFO waits without an exec; m5 cannot open it and sleeps.
	fifo=${TICK_FIFO:-/run/.bootsync-tick}
	n=0
	while autorun_alive "$pid" && [ "$n" -lt 6 ]; do
		if [ -p "$fifo" ] && [ -w "$fifo" ]; then
			read -r -t 1 _ 2>/dev/null 0<> "$fifo" || true
		else
			sleep 1
		fi
		n=$((n + 1))
	done
	autorun_alive "$pid" && autorun_signal KILL "$pid"
	: > "$AUTORUN_PID"
	echo "--- autorun: stopped" >> "$AUTORUN_LOG"
	es_log "autorun: stopped pid $pid"
}

# Pick the program in AUTORUN_DIR and start it in the background.
autorun_start() {
	autorun_running && return 0
	[ -f "$AUTORUN_DIR/$AUTORUN_SETTINGS" ] || return 0
	mode=auto
	restart=0
	read -r m r 2>/dev/null < "$AUTORUN_DIR/$AUTORUN_SETTINGS" || true
	mode=${m#mode=}
	restart=${r#restart=}
	if [ "$mode" = off ]; then
		es_log "autorun: off (autorun=off in config.txt)"
		return 0
	fi
	prog=""
	if [ "$mode" = auto ]; then
		for n in main.py main.sh main; do
			[ -f "$AUTORUN_DIR/$n" ] && { prog=$n; break; }
		done
		[ -n "$prog" ] || return 0
	else
		prog=$mode
		if [ ! -f "$AUTORUN_DIR/$prog" ]; then
			es_log "autorun: $prog (autorun= in config.txt) is not on the drive"
			return 0
		fi
	fi

	case "$prog" in
		*.py) set -- python3 "$prog" ;;
		*.sh) set -- sh "$prog" ;;
		*)
			# Only NOMMU flat binaries run here; an ELF fails with ENOEXEC.
			magic=""
			head -c 4 "$AUTORUN_DIR/$prog" > /run/.autorun-magic 2>/dev/null
			read -r magic 2>/dev/null < /run/.autorun-magic || true
			rm -f /run/.autorun-magic
			if [ "$magic" != bFLT ]; then
				echo "--- autorun: $prog is not a riscv32 NOMMU bFLT binary (elf2flt); not started" >> "$AUTORUN_LOG"
				es_log "autorun: $prog is not a bFLT binary; not started"
				return 1
			fi
			chmod 755 "$AUTORUN_DIR/$prog"
			set -- "./$prog"
			;;
	esac
	[ "$restart" = 1 ] && set -- /usr/sbin/respawn -d 5 -- "$@"
	if autorun_as_root; then
		set -- chpst -u "$AUTORUN_USER" -P -n 10 "$@"
	else
		set -- chpst -P -n 10 "$@"
	fi
	echo "--- autorun: start $prog" >> "$AUTORUN_LOG"
	cd "$AUTORUN_DIR" || return 1
	# A login's umask, not the 077 that bootsyncd runs under.
	(
		umask 022
		export HOME=/home/$AUTORUN_USER USER=$AUTORUN_USER LOGNAME=$AUTORUN_USER
		export MICROPYPATH="$AUTORUN_DIR/lib:.frozen:/home/$AUTORUN_USER/.micropython/lib:/usr/lib/micropython"
		exec "$@"
	) >> "$AUTORUN_LOG" 2>&1 < /dev/null &
	echo "$!" > "$AUTORUN_PID"
	cd /
	es_log "autorun: started $prog pid $!"
}

# Swap in AUTORUN_NEW when it differs from the running copy and restart.
autorun_apply() {
	[ -d "$AUTORUN_NEW" ] || return 0
	# The main BusyBox find has no -type: md5sum skips the directories.
	find "$AUTORUN_NEW" | sort | xargs md5sum 2>/dev/null | md5sum > "$AUTORUN_SUM.new"
	new=""
	old=""
	read -r new _ 2>/dev/null < "$AUTORUN_SUM.new" || true
	read -r old _ 2>/dev/null < "$AUTORUN_SUM" || true
	if [ -n "$new" ] && [ "$new" = "$old" ] && [ -d "$AUTORUN_DIR" ]; then
		rm -rf "$AUTORUN_NEW" "$AUTORUN_SUM.new"
		return 0
	fi
	autorun_stop
	rm -rf "$AUTORUN_DIR"
	mv -f "$AUTORUN_NEW" "$AUTORUN_DIR" || return 1
	mv -f "$AUTORUN_SUM.new" "$AUTORUN_SUM"
	autorun_start
}

# Copy-truncate the log past AUTORUN_LOG_MAX (the program keeps its O_APPEND
# descriptor).  wc runs only when the log changed.
autorun_trim_log() {
	[ -f "$AUTORUN_LOG" ] || return 0
	if [ -f "$AUTORUN_LOG_REF" ]; then
		[ "$AUTORUN_LOG" -nt "$AUTORUN_LOG_REF" ] || return 0
	fi
	touch -r "$AUTORUN_LOG" "$AUTORUN_LOG_REF"
	size=0
	wc -c < "$AUTORUN_LOG" > /run/.autorun-size 2>/dev/null
	read -r size 2>/dev/null < /run/.autorun-size || true
	[ "${size:-0}" -gt "$AUTORUN_LOG_MAX" ] || return 0
	cp -f "$AUTORUN_LOG" "$AUTORUN_LOG.1" && chmod 644 "$AUTORUN_LOG.1"
	: > "$AUTORUN_LOG"
}
