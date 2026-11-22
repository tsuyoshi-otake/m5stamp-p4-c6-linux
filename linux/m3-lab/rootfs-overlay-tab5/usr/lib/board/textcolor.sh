# textcolor.sh: the panel-console text colour presets the m5-config
# text_color key picks from.  Sourced by S07textcolor (boot + live
# apply) and by m5-config (validation, the menu pick, the palette
# restore at session end).  Builtins only, no $( ), no external
# command: it also runs inside the touch menu's session teardown
# where the pool has ~1.5 MB left on NOMMU.
#
# A non-white preset turns the Linux VT into a two-intensity
# monochrome display, the way the amber and green terminals were:
# slot 0 the background, slots 1-7 the normal colour, slot 8 a dim
# "bright black" shade, slots 9-F the bright (bold) colour -- every
# ANSI colour maps to the one hue.  The background is black, except
# for 'lcd': dark ink on a lit background, the reflective LCD of an
# old handheld.  'white' is the stock 16-colour palette (ESC ] R).

TEXTCOLOR_NAMES="white green amber orange cyan blue pink red lcd"

# fbcon's rotation attribute: textcolor_redraw writes its value back
# to it.
TEXTCOLOR_FBROT=${M5CFG_FBCON_ROTATE:-/sys/class/graphics/fbcon/rotate}

# _tc_mono NORMAL BRIGHT DIM [BACKGROUND] -> TC_SEQ, TC_HEX, TC_BG.
# TC_SEQ is printf FORMAT text (the \033 below stays the four
# characters backslash-0-3-3 until the caller's printf "$TC_SEQ"
# turns it into the ESC byte); TC_HEX is the normal colour rrggbb and
# TC_BG the background rrggbb (000000 unless given) -- the menu's
# sample line uses both.  Slot digits are the upper-case hex
# m5-config's _pal_set writes.
_tc_mono() {
	TC_HEX=$1
	TC_BG=${4:-000000}
	TC_SEQ="\033]P0$TC_BG"
	_i=1
	while [ "$_i" -lt 8 ]; do
		TC_SEQ="$TC_SEQ\033]P${_i}$1"
		_i=$((_i + 1))
	done
	TC_SEQ="$TC_SEQ\033]P8$3"
	TC_SEQ="$TC_SEQ\033]P9$2\033]PA$2\033]PB$2\033]PC$2"
	TC_SEQ="$TC_SEQ\033]PD$2\033]PE$2\033]PF$2"
}

# textcolor_seq NAME -> TC_SEQ, TC_HEX, TC_BG (see above); rc 1 and
# all three empty for an unknown name.  One line per preset: NORMAL
# BRIGHT DIM [BACKGROUND].  Each black-background row is one base
# colour at 84 % (normal text is not the tube's full drive), the base
# lifted 10 % toward white (bold) and the base at 45 % (dim).  amber
# aims at a DEC VT320-C2; its base ff8800 was chosen on the Tab5
# panel through a camera, where the usual "amber" ffb000 shows as
# yellow.  lcd is the other way round: on a lit background bold has
# to be the darker ink (the ink at 55 %), and dim sits about half way
# from the background to the ink.  It aims at the yellow-green STN of
# an HP 95LX/200LX and was picked by eye on the Tab5 panel with a
# real reflective LCD beside it, in a warm-lit room: the backlight is
# far bluer and brighter than paper there, so the background that
# reads as dull yellow-green on the panel is a dark olive brown as a
# number.  Do not "correct" it from a colour picker.
textcolor_seq() {
	TC_SEQ=""; TC_HEX=""; TC_BG=""
	case "$1" in
		green)	_tc_mono 2bd62b 47ff47 177317 ;;
		amber)	_tc_mono d67200 ff941a 733d00 ;;
		orange)	_tc_mono d65400 ff741a 732d00 ;;
		cyan)	_tc_mono 2bbcd6 47e3ff 176573 ;;
		blue)	_tc_mono 598bd6 79aeff 304a73 ;;
		pink)	_tc_mono d65ca7 ff7ccd 73325a ;;
		red)	_tc_mono d63636 ff5353 731d1d ;;
		lcd)	_tc_mono 11100d 090907 2d271b 4e422c ;;
		white)	TC_SEQ='\033]R'; TC_HEX=aaaaaa; TC_BG=000000 ;;
		*)	return 1 ;;
	esac
	return 0
}

# textcolor_redraw: make a palette that was just written show.  On
# this truecolor framebuffer the cells already drawn keep their old
# colours, and the margin outside the text rows (16 px on the Tab5:
# 720 px is 22.5 rows of 32) is only filled with slot 0 when fbcon
# repaints the whole screen.  Writing fbcon's rotation back to itself
# makes it do that -- one repaint of every cell and of the margin,
# no exec.  Without the attribute the DECSCNM on/off pair goes to
# stdout, which the caller points at the panel tty: the VT redraws
# the cells (one flash) and the margin keeps the colour it had.
textcolor_redraw() {
	_r=""
	[ -w "$TEXTCOLOR_FBROT" ] &&
		read -r _r 2>/dev/null < "$TEXTCOLOR_FBROT"
	case "$_r" in
		[0-3])
			echo "$_r" 2>/dev/null > "$TEXTCOLOR_FBROT" &&
				return 0 ;;
	esac
	printf '\033[?5h\033[?5l'
}
