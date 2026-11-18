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
 * an open fd or -1.  When path is not NULL it receives the node name. */
static int open_touch(char *path, size_t plen)
{
	char buf[40];
	unsigned long bits[16];
	int i, fd;

	for (i = 0; i < 16; i++) {
		snprintf(buf, sizeof(buf), "/dev/input/event%d", i);
		fd = open(buf, O_RDONLY | O_NONBLOCK);
		if (fd < 0)
			continue;
		memset(bits, 0, sizeof(bits));
		if (ioctl(fd, EVIOCGBIT(EV_KEY, sizeof(bits)), bits) > 0 &&
		    (bits[BTN_TOUCH / 64] & (1UL << (BTN_TOUCH % 64)))) {
			memset(bits, 0, sizeof(bits));
			if (ioctl(fd, EVIOCGBIT(EV_ABS, sizeof(bits)), bits) > 0 &&
			    (bits[ABS_X / 64] & (1UL << (ABS_X % 64))) &&
			    (bits[ABS_Y / 64] & (1UL << (ABS_Y % 64)))) {
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

static long ms_now(void)
{
	struct timeval tv;

	gettimeofday(&tv, NULL);
	return tv.tv_sec * 1000L + tv.tv_usec / 1000;
}

int main(int argc, char *argv[])
{
	static struct input_absinfo abx, aby;
	static struct fb_var_screeninfo var;
	static struct input_event evs[16];
	static struct termios saved, raw;
	struct winsize ws;
	char tpath[40];
	int tfd = -1, fbfd = -1, rot, max_x, max_y;
	int ax = -1, ay = -1, touching = 0;
	long down_ms = 0;
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
		int row, col;

		if (tap_to_cell(atoi(argv[2]), atoi(argv[3]),
				atoi(argv[4]), atoi(argv[5]),
				atoi(argv[6]), atoi(argv[7]),
				atoi(argv[8]), &row, &col) == 0) {
			printf("%d %d\n", row, col);
			return 0;
		}
		return 1;
	}
	if (argc != 1) {
		fputs("usage: m5-key | m5-key probe | "
		      "m5-key map XRES YRES ROT COLS ROWS PX PY\n",
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
	rot = read_int_file(ROTATE_NODE, 0);
	if (rot < 0 || rot > 3)
		rot = 0;

	max_x = abx.maximum > 0 ? abx.maximum : (int)var.xres - 1;
	max_y = aby.maximum > 0 ? aby.maximum : (int)var.yres - 1;

	tcgetattr(0, &saved);
	raw = saved;
	raw.c_lflag &= ~(ICANON | ECHO);
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
				tcsetattr(0, TCSANOW, &saved);
				printf("K %c\n", c >= 32 && c < 127 ? c : '?');
				return 0;
			}
			break;
		}
		if (FD_ISSET(tfd, &rfds)) {
			ssize_t n = read(tfd, evs, sizeof(evs));
			int off;

			if (n <= 0)
				continue;
			for (off = 0;
			     off + (int)sizeof(struct input_event) <= n;
			     off += (int)sizeof(struct input_event)) {
				const struct input_event *e =
					(const struct input_event *)
					((const char *)evs + off);
				int was = touching;

				if (e->type == EV_ABS) {
					if (e->code == ABS_X ||
					    e->code == ABS_MT_POSITION_X)
						ax = e->value;
					else if (e->code == ABS_Y ||
						 e->code == ABS_MT_POSITION_Y)
						ay = e->value;
					if (e->code == ABS_MT_TRACKING_ID) {
						touching = e->value >= 0;
						if (touching && !was)
							down_ms = ms_now();
					}
				} else if (e->type == EV_KEY &&
					   e->code == BTN_TOUCH) {
					touching = e->value != 0;
					if (touching && !was)
						down_ms = ms_now();
				} else if (e->type == EV_SYN &&
					   e->code == SYN_REPORT &&
					   was && !touching &&
					   ax >= 0 && ay >= 0) {
					int px, py, row, col;

					if (ms_now() - down_ms < MIN_TOUCH_MS)
						continue;
					px = ax * ((int)var.xres - 1) / max_x;
					py = ay * ((int)var.yres - 1) / max_y;
					if (tap_to_cell((int)var.xres,
							(int)var.yres, rot,
							ws.ws_col, ws.ws_row,
							px, py,
							&row, &col) == 0) {
						tcsetattr(0, TCSANOW, &saved);
						printf("T %d %d\n", row, col);
						return 0;
					}
				}
			}
		}
	}
	tcsetattr(0, TCSANOW, &saved);
	return stopped ? 2 : 1;
}
