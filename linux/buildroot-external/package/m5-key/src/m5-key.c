/*
 * m5-key: one console input event for m5-config's touch menus (#42).
 *
 * Polls the controlling tty (raw bytes) and the touchscreen evdev node
 * at once; prints a single event on stdout and exits:
 *
 *   K <char>        a byte from the tty (keyboard, serial, A164, BLE)
 *   T <row> <col>   a finger tap, as a console cell
 *
 * The tap is reported on release (BTN_TOUCH up, or MT tracking-id -1
 * plus SYN_REPORT) so a resting finger produces exactly one event.
 * Coordinates run through EVIOCGABS ranges -> /dev/fb0 pixels -> the
 * fbcon rotation (/sys/class/graphics/fbcon/rotate) -> console cells
 * (TIOCGWINSZ), the same chain touch-dots uses for the ABS->fb step.
 *
 *   m5-key            one event, as above
 *   m5-key probe      print the touch device path; exit 1 when absent
 *   m5-key map XRES YRES ROT COLS ROWS PX PY
 *                     print "ROW COL" for a pixel (host-test seam)
 *   m5-key feed FILE XRES YRES ROT COLS ROWS MAXX MAXY
 *                     read struct input_event records from FILE
 *                     (- = stdin) instead of the evdev node; the same
 *                     state machine a live run uses (host-test seam)
 *   m5-key kmap BYTE  print the K token BYTE (decimal) would produce
 *                     (host-test seam; nothing for a byte that is
 *                     not reported)
 *
 * Exit: 0 an event was printed, 1 setup failed, 2 interrupted.
 * Host-buildable for tests: cc -o m5-key m5-key.c
 */

#include <errno.h>
#include <fcntl.h>
#include <linux/fb.h>
#include <linux/input.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/select.h>
#include <sys/time.h>
#include <termios.h>
#include <unistd.h>

#define ROTATE_NODE "/sys/class/graphics/fbcon/rotate"
#define FB0 "/dev/fb0"

/* Report taps on release; ignore contacts shorter than this (jitter
 * from a resting palm edge) so one press is one event. */
#define MIN_TOUCH_MS 30

static volatile sig_atomic_t stopped;

static void on_stop(int sig)
{
	(void)sig;
	stopped = 1;
}

/* Find the first evdev node that has BTN_TOUCH + ABS_X/ABS_Y.  Returns
 * an open fd or -1.  When path is not NULL it receives the node name.
 * The capability bitmaps are byte arrays: unsigned long is 32 bits on
 * RV32, so /64 indexing would probe the wrong bit. */
static int open_touch(char *path, size_t plen)
{
	char buf[40];
	unsigned char bits[(KEY_MAX + 8) / 8];
	int i, fd;

	for (i = 0; i < 16; i++) {
		snprintf(buf, sizeof(buf), "/dev/input/event%d", i);
		fd = open(buf, O_RDONLY | O_NONBLOCK);
		if (fd < 0)
			continue;
		memset(bits, 0, sizeof(bits));
		if (ioctl(fd, EVIOCGBIT(EV_KEY, sizeof(bits)), bits) > 0 &&
		    (bits[BTN_TOUCH / 8] & (1 << (BTN_TOUCH % 8)))) {
			memset(bits, 0, sizeof(bits));
			if (ioctl(fd, EVIOCGBIT(EV_ABS, sizeof(bits)),
				  bits) > 0 &&
			    (bits[ABS_X / 8] & (1 << (ABS_X % 8))) &&
			    (bits[ABS_Y / 8] & (1 << (ABS_Y % 8)))) {
				if (path)
					snprintf(path, plen, "%s", buf);
				return fd;
			}
		}
		close(fd);
	}
	return -1;
}

static int read_int_file(const char *path, int dflt)
{
	char buf[16];
	int fd = open(path, O_RDONLY);
	ssize_t n;

	if (fd < 0)
		return dflt;
	n = read(fd, buf, sizeof(buf) - 1);
	close(fd);
	if (n <= 0)
		return dflt;
	buf[n] = 0;
	return atoi(buf);
}

/*
 * tap_to_cell: physical fb pixel -> console cell.
 * pw/ph: fb xres/yres.  rot: fbcon rotate (0 normal, 1 CW, 2 UD,
 * 3 CCW).  cols/rows: TIOCGWINSZ answer.  Returns 0 and fills
 * the row/col out-params, or -1 when the geometry is nonsense.
 */
static int tap_to_cell(int pw, int ph, int rot, int cols, int rows,
		       int px, int py, int *row, int *col)
{
	long lw = (rot & 1) ? ph : pw;  /* logical width  in pixels */
	long lh = (rot & 1) ? pw : ph;  /* logical height in pixels */
	long lx, ly;

	if (pw <= 0 || ph <= 0 || cols <= 0 || rows <= 0)
		return -1;
	switch (rot) {
	case 0: lx = px;           ly = py;           break;
	case 1: lx = (long)py;     ly = lh - 1 - px;  break;
	case 2: lx = pw - 1 - px;  ly = ph - 1 - py;  break;
	case 3: lx = lw - 1 - py;  ly = px;           break;
	default:
		return -1;
	}
	if (lx < 0 || ly < 0 || lx >= lw || ly >= lh)
		return -1;
	*col = (int)(lx * cols / lw);
	*row = (int)(ly * rows / lh);
	return 0;
}

/* Touch state for one run; the feed loop below is the single place
 * that updates it, for both the evdev and the file-driven paths. */
static int ax = -1, ay = -1;          /* last abs position */
static int touching;                  /* any contact down right now */
static int btn_touch;                 /* BTN_TOUCH state (type-A path) */
static unsigned slots_down;           /* MT-B per-slot contact mask */
static int cur_slot;                  /* ABS_MT_SLOT for MT events */
static int prev_frame_touching;       /* contact at last SYN_REPORT */
static long down_sec, down_usec;
static int geometry[7];               /* xres yres rot cols rows maxx maxy */

static long ms_between(long s0, long u0, long s1, long u1)
{
	return (s1 - s0) * 1000L + (u1 - u0) / 1000;
}

/*
 * One input event through the state machine.  A frame = events up to a
 * SYN_REPORT; the release is reported when prev_frame_touching goes
 * 1 -> 0 (BTN_TOUCH-up and SYN_REPORT are different events, so a
 * per-event "was" never sees the transition).  Returns 1 and fills
 * row/col when a tap completes, 0 otherwise.
 *
 * MT protocol B (the Tab5 st7123-touch): every contact lives in an
 * ABS_MT_SLOT, so a TRACKING_ID -1 only clears that slot's bit -- a
 * second finger lifting while the first stays down must not report a
 * tap.  Slot>0 positions never overwrite ax/ay; the pointer-emulation
 * ABS_X/ABS_Y events that the real device sends in the same frame stay
 * authoritative for the release position.
 */
static int feed_event(const struct input_event *e, int *row, int *col)
{
	if (e->type == EV_ABS) {
		if (e->code == ABS_MT_SLOT) {
			if (e->value >= 0 && e->value < 32)
				cur_slot = e->value;
		} else if (e->code == ABS_X ||
			   (e->code == ABS_MT_POSITION_X &&
			    cur_slot == 0)) {
			ax = e->value;
		} else if (e->code == ABS_Y ||
			   (e->code == ABS_MT_POSITION_Y &&
			    cur_slot == 0)) {
			ay = e->value;
		} else if (e->code == ABS_MT_TRACKING_ID) {
			if (e->value >= 0 && !touching) {
				down_sec = e->input_event_sec;
				down_usec = e->input_event_usec;
			}
			if (e->value >= 0)
				slots_down |= 1U << cur_slot;
			else
				slots_down &= ~(1U << cur_slot);
			touching = btn_touch || slots_down != 0;
		}
	} else if (e->type == EV_KEY && e->code == BTN_TOUCH) {
		if (e->value && !touching) {
			down_sec = e->input_event_sec;
			down_usec = e->input_event_usec;
		}
		btn_touch = e->value != 0;
		touching = btn_touch || slots_down != 0;
	} else if (e->type == EV_SYN && e->code == SYN_REPORT) {
		int released = prev_frame_touching && !touching;
		int px, py;

		prev_frame_touching = touching;
		if (!released || ax < 0 || ay < 0)
			return 0;
		/* the event's own timestamp, not wall time: a file-fed
		 * replay must keep the same duration semantics */
		if (ms_between(down_sec, down_usec,
			       e->input_event_sec,
			       e->input_event_usec) < MIN_TOUCH_MS)
			return 0;
		px = ax * (geometry[0] - 1) / geometry[5];
		py = ay * (geometry[1] - 1) / geometry[6];
		return tap_to_cell(geometry[0], geometry[1], geometry[2],
				   geometry[3], geometry[4],
				   px, py, row, col) == 0;
	}
	return 0;
}

/*
 * One tty byte -> one K token.  Printable bytes keep the "K <char>"
 * form (a literal '?' included); Enter and Backspace get names so the
 * menu can tell them apart from a '?' that starts a passphrase.  Any
 * other control byte cannot be a menu pick and reports nothing --
 * the caller then keeps polling.  Returns 1 when a token was printed.
 */
static int emit_key(int c)
{
	if (c == '\r' || c == '\n')
		puts("K enter");
	else if (c == '\b' || c == 0x7f)
		puts("K bs");
	else if (c >= 32 && c < 127)
		printf("K %c\n", c);
	else
		return 0;
	return 1;
}

static int feed_stream(int fd)
{
	static struct input_event evs[16];
	ssize_t n;
	int off, row, col;

	while (!stopped && (n = read(fd, evs, sizeof(evs))) > 0) {
		for (off = 0; off + (int)sizeof(struct input_event) <= n;
		     off += (int)sizeof(struct input_event)) {
			const struct input_event *e =
				(const struct input_event *)
				((const char *)evs + off);

			if (feed_event(e, &row, &col)) {
				printf("T %d %d\n", row, col);
				return 1;
			}
		}
	}
	return 0;
}

int main(int argc, char *argv[])
{
	static struct input_absinfo abx, aby;
	static struct fb_var_screeninfo var;
	static struct termios saved, raw;
	struct winsize ws;
	char tpath[40];
	int tfd = -1, fbfd = -1, row, col;
	fd_set rfds;
	int r;

	if (argc >= 2 && strcmp(argv[1], "probe") == 0) {
		tfd = open_touch(tpath, sizeof(tpath));
		if (tfd < 0)
			return 1;
		printf("%s\n", tpath);
		return 0;
	}
	if (argc == 9 && strcmp(argv[1], "map") == 0) {
		if (tap_to_cell(atoi(argv[2]), atoi(argv[3]),
				atoi(argv[4]), atoi(argv[5]),
				atoi(argv[6]), atoi(argv[7]),
				atoi(argv[8]), &row, &col) == 0) {
			printf("%d %d\n", row, col);
			return 0;
		}
		return 1;
	}
	if (argc == 10 && strcmp(argv[1], "feed") == 0) {
		int ffd;

		geometry[0] = atoi(argv[3]);
		geometry[1] = atoi(argv[4]);
		geometry[2] = atoi(argv[5]);
		geometry[3] = atoi(argv[6]);
		geometry[4] = atoi(argv[7]);
		geometry[5] = atoi(argv[8]);
		geometry[6] = atoi(argv[9]);
		ffd = strcmp(argv[2], "-") == 0 ? 0 : open(argv[2], O_RDONLY);
		if (ffd < 0)
			return 1;
		setvbuf(stdout, NULL, _IONBF, 0);
		r = feed_stream(ffd);
		return r ? 0 : 1;
	}
	if (argc == 3 && strcmp(argv[1], "kmap") == 0)
		return emit_key(atoi(argv[2])) ? 0 : 1;
	if (argc != 1) {
		fputs("usage: m5-key | m5-key probe | "
		      "m5-key map XRES YRES ROT COLS ROWS PX PY | "
		      "m5-key feed FILE XRES YRES ROT COLS ROWS MAXX MAXY | "
		      "m5-key kmap BYTE\n",
		      stderr);
		return 1;
	}

	tfd = open_touch(tpath, sizeof(tpath));
	if (tfd < 0) {
		fputs("m5-key: no touchscreen found\n", stderr);
		return 1;
	}
	if (ioctl(tfd, EVIOCGABS(ABS_X), &abx) < 0 ||
	    ioctl(tfd, EVIOCGABS(ABS_Y), &aby) < 0) {
		fputs("m5-key: EVIOCGABS failed\n", stderr);
		return 1;
	}

	fbfd = open(FB0, O_RDONLY);
	if (fbfd < 0 || ioctl(fbfd, FBIOGET_VSCREENINFO, &var) < 0) {
		fprintf(stderr, "m5-key: %s: %s\n", FB0, strerror(errno));
		return 1;
	}
	if (ioctl(0, TIOCGWINSZ, &ws) < 0 || ws.ws_col == 0 ||
	    ws.ws_row == 0) {
		fputs("m5-key: stdin is not a console tty\n", stderr);
		return 1;
	}
	r = read_int_file(ROTATE_NODE, 0);
	if (r < 0 || r > 3)
		r = 0;
	geometry[0] = (int)var.xres;
	geometry[1] = (int)var.yres;
	geometry[2] = r;
	geometry[3] = ws.ws_col;
	geometry[4] = ws.ws_row;
	geometry[5] = abx.maximum > 0 ? abx.maximum : (int)var.xres - 1;
	geometry[6] = aby.maximum > 0 ? aby.maximum : (int)var.yres - 1;

	tcgetattr(0, &saved);
	raw = saved;
	raw.c_lflag &= ~(ICANON | ECHO);
	raw.c_cc[VMIN] = 1;
	raw.c_cc[VTIME] = 0;
	tcsetattr(0, TCSANOW, &raw);
	signal(SIGINT, on_stop);
	signal(SIGTERM, on_stop);
	setvbuf(stdout, NULL, _IONBF, 0);

	while (!stopped) {
		FD_ZERO(&rfds);
		FD_SET(0, &rfds);
		FD_SET(tfd, &rfds);
		r = select(tfd + 1, &rfds, NULL, NULL, NULL);
		if (r < 0) {
			if (errno == EINTR)
				continue;
			break;
		}
		if (FD_ISSET(0, &rfds)) {
			unsigned char c;

			if (read(0, &c, 1) == 1) {
				if (c == 0x1b) {
					/* swallow the rest of an escape
					 * sequence (arrow keys etc.) so
					 * '[' 'A' are not menu picks */
					char junk[8];
					struct timeval tv = { 0, 50000 };
					fd_set ef;

					FD_ZERO(&ef);
					FD_SET(0, &ef);
					while (select(1, &ef, NULL, NULL,
						      &tv) > 0 &&
					       read(0, junk, sizeof(junk)) > 0) {
						FD_ZERO(&ef);
						FD_SET(0, &ef);
						tv.tv_sec = 0;
						tv.tv_usec = 50000;
					}
					continue;
				}
				if (emit_key(c)) {
					tcsetattr(0, TCSANOW, &saved);
					return 0;
				}
				continue;
			}
			break;
		}
		if (FD_ISSET(tfd, &rfds) && feed_stream(tfd)) {
			tcsetattr(0, TCSANOW, &saved);
			return 0;
		}
	}
	tcsetattr(0, TCSANOW, &saved);
	return stopped ? 2 : 1;
}
