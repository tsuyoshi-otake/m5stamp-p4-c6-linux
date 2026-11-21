# textcolor.sh: the panel-console text colour presets the m5-config
# text_color key picks from.  Sourced by S07textcolor (boot + live
# apply) and by m5-config (validation, the menu pick, the palette
# restore at session end).  Builtins only, no $( ), no external
# command: it also runs inside the touch menu's session teardown
# where the pool has ~1.5 MB left on NOMMU.
#
# A non-white preset turns the Linux VT into a two-intensity
# monochrome display, the way the amber and green terminals were:
# slot 0 black, slots 1-7 the normal colour, slot 8 a dim "bright
# black" shade, slots 9-F the bright (bold) colour -- every ANSI
# colour maps to the one hue.  'white' is the stock 16-colour
# palette (ESC ] R).

TEXTCOLOR_NAMES="white green amber orange cyan blue pink red"

# _tc_mono NORMAL BRIGHT DIM -> TC_SEQ, TC_HEX.  TC_SEQ is printf
# FORMAT text (the \033 below stays the four characters
# backslash-0-3-3 until the caller's printf "$TC_SEQ" turns it into
# the ESC byte); TC_HEX is the normal colour rrggbb, used by the
# menu's sample line.  Slot digits are the upper-case hex m5-config's
# _pal_set writes.
_tc_mono() {
	TC_HEX=$1
	TC_SEQ='\033]P0000000'
	_i=1
	while [ "$_i" -lt 8 ]; do
		TC_SEQ="$TC_SEQ\033]P${_i}$1"
		_i=$((_i + 1))
	done
	TC_SEQ="$TC_SEQ\033]P8$3"
	TC_SEQ="$TC_SEQ\033]P9$2\033]PA$2\033]PB$2\033]PC$2"
	TC_SEQ="$TC_SEQ\033]PD$2\033]PE$2\033]PF$2"
}

# textcolor_seq NAME -> TC_SEQ, TC_HEX (see above); rc 1 and both
# empty for an unknown name.  One line per preset: NORMAL BRIGHT DIM.
# Each row is one base colour at 84 % (normal text is not the tube's
# full drive), the base lifted 10 % toward white (bold) and the base
# at 45 % (dim).  amber aims at a DEC VT320-C2; its base ff8800 was
# chosen on the Tab5 panel through a camera, where the usual "amber"
# ffb000 shows as yellow.
textcolor_seq() {
	TC_SEQ=""; TC_HEX=""
	case "$1" in
		green)	_tc_mono 2bd62b 47ff47 177317 ;;
		amber)	_tc_mono d67200 ff941a 733d00 ;;
		orange)	_tc_mono d65400 ff741a 732d00 ;;
		cyan)	_tc_mono 2bbcd6 47e3ff 176573 ;;
		blue)	_tc_mono 598bd6 79aeff 304a73 ;;
		pink)	_tc_mono d65ca7 ff7ccd 73325a ;;
		red)	_tc_mono d63636 ff5353 731d1d ;;
		white)	TC_SEQ='\033]R'; TC_HEX=aaaaaa ;;
		*)	return 1 ;;
	esac
	return 0
}
