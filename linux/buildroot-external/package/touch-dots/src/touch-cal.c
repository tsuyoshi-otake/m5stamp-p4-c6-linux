/*
 * touch-cal: 5-point touchscreen calibrator for the Tab5 panel (#33).
 *
 * Draws a numbered crosshair on /dev/fb0 at each of five targets (the
 * four corners a little inside, then centre), waits for a tap, and
 * records the averaged reported position of that tap.  With all five
 * (reported -> target) pairs it solves a least-squares affine map
 *
 *	fb_x = a*x + b*y + c
 *	fb_y = d*x + e*y + f
 *
 * and saves the six coefficients to touch-cal.txt on the boot drive
 * (via boot-edit --put; /boot itself may be read-only after boot)
 * in the key=value style ("touch_cal=a,b,c,d,e,f").  bootsyncd then
 * publishes it as /tmp/touch-cal.txt, which touch-dots reads and
 * applies when it exists.
 *
 * Prints the coefficients and the residual error in px per point,
 * then restores the text console and exits.
 */

#include <errno.h>
#include <fcntl.h>
#include <linux/fb.h>
#include <linux/input.h>
#include <linux/kd.h>
#include <math.h>
#include <poll.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <time.h>
#include <unistd.h>

#define INPUT_DEV "/dev/input/event0"
#define FB0 "/dev/fb0"
#define TTY0 "/dev/tty0"
#define TTYCON "/dev/console"
#define CAL_NAME "touch-cal.txt"
#define CAL_TMP "/tmp/touch-cal.txt"
#define CAL_PUT "boot-edit --put " CAL_NAME

#define N_TARGETS 5
#define MARGIN 40
#define CROSS_R 14

static struct fb_var_screeninfo var;
static struct fb_fix_screeninfo fix;
static int bypp;
static int fbfd = -1;
static int kdfd = -1;
static volatile sig_atomic_t stopped;

/* 3x5 digits 1..5, one bit per pixel, MSB left. */
static const uint8_t digits[N_TARGETS][5] = {
	{ 0x2, 0x6, 0x2, 0x2, 0x7 },	/* 1 */
	{ 0x7, 0x1, 0x7, 0x4, 0x7 },	/* 2 */
	{ 0x7, 0x1, 0x7, 0x1, 0x7 },	/* 3 */
	{ 0x5, 0x5, 0x7, 0x1, 0x1 },	/* 4 */
	{ 0x7, 0x4, 0x7, 0x1, 0x7 },	/* 5 */
};

static void on_stop(int sig)
{
	(void)sig;
	stopped = 1;
}

static int console_fd(void)
{
	int fd = open(TTY0, O_WRONLY | O_NOCTTY);

	if (fd < 0)
		fd = open(TTYCON, O_WRONLY | O_NOCTTY);
	return fd;
}

static void kd_text(int fd)
{
	if (fd >= 0 && ioctl(fd, KDSETMODE, KD_TEXT) < 0) {
		ssize_t w = write(fd, "\f", 1);
		(void)w;
	}
}

static void put_px(int x, int y, int white)
{
	uint8_t px[4] = { 0, 0, 0, 0 };
	off_t off;

	if (x < 0 || y < 0 || x >= (int)var.xres || y >= (int)var.yres)
		return;
	if (white) {
		if (var.bits_per_pixel == 16) {
			px[0] = 0xff;
			px[1] = 0xff;
		} else {
			px[0] = px[1] = px[2] = 0xff;
		}
	}
	off = (off_t)fix.line_length * y + (off_t)x * bypp;
	pwrite(fbfd, px, bypp, off);
}

static void fill_fb(int white)
{
	int x, y;

	for (y = 0; y < (int)var.yres; y++)
		for (x = 0; x < (int)var.xres; x++)
			put_px(x, y, white);
	ioctl(fbfd, FBIOPAN_DISPLAY, &var);
}

static void draw_target(int i, int tx, int ty)
{
	int x, y;

	fill_fb(0);
	for (x = tx - CROSS_R; x <= tx + CROSS_R; x++) {
		put_px(x, ty, 1);
		put_px(x, ty + 1, 1);
	}
	for (y = ty - CROSS_R; y <= ty + CROSS_R; y++) {
		put_px(tx, y, 1);
		put_px(tx + 1, y, 1);
	}
	for (y = 0; y < 5; y++)
		for (x = 0; x < 3; x++)
			if (digits[i][y] & (1 << (2 - x))) {
				put_px(tx + 20 + x, ty - 6 + y, 1);
				put_px(tx + 20 + x + 3, ty - 6 + y, 1);
			}
	ioctl(fbfd, FBIOPAN_DISPLAY, &var);
}

/* Discard input until the finger has been up for 300 ms.  A tap is
 * often followed by a second, spurious contact (or the tail of the
 * same press), which would otherwise advance the next target to the
 * same place. */
static void wait_release_quiet(int infd)
{
	struct input_event evs[16];
	struct timespec rel;
	ssize_t n;

	clock_gettime(CLOCK_MONOTONIC, &rel);
	for (;;) {
		struct timespec now;
		long ms;
		struct pollfd p = { .fd = infd, .events = POLLIN };
		int r = poll(&p, 1, 350);

		if (stopped)
			return;
		if (r <= 0)
			return;
		n = read(infd, evs, sizeof(evs));
		if (n <= 0)
			continue;
		for (int off = 0; off + (int)sizeof(*evs) <= n;
		     off += (int)sizeof(*evs)) {
			const struct input_event *e =
				(const struct input_event *)
				((const char *)evs + off);
			if ((e->type == EV_KEY && e->code == BTN_TOUCH) ||
			    (e->type == EV_ABS &&
			     e->code == ABS_MT_TRACKING_ID))
				clock_gettime(CLOCK_MONOTONIC, &rel);
		}
		clock_gettime(CLOCK_MONOTONIC, &now);
		ms = (now.tv_sec - rel.tv_sec) * 1000 +
		     (now.tv_nsec - rel.tv_nsec) / 1000000;
		if (ms >= 300)
			return;
	}
}

/* Wait for one tap; returns the averaged reported position. */
static int read_tap(int infd, int *ox, int *oy)
{
	struct input_event evs[16];
	long sx = 0, sy = 0, npts = 0;
	int ax = -1, ay = -1, touching = 0;
	ssize_t n;

	wait_release_quiet(infd);
	for (;;) {
		n = read(infd, evs, sizeof(evs));
		if (n <= 0) {
			if (stopped || n < 0)
				return -1;
			continue;
		}
		for (int off = 0; off + (int)sizeof(*evs) <= n;
		     off += (int)sizeof(*evs)) {
			const struct input_event *e =
				(const struct input_event *)
				((const char *)evs + off);

			if (e->type == EV_ABS) {
				if (e->code == ABS_X ||
				    e->code == ABS_MT_POSITION_X)
					ax = e->value;
				else if (e->code == ABS_Y ||
					 e->code == ABS_MT_POSITION_Y)
					ay = e->value;
				if (e->code == ABS_MT_TRACKING_ID)
					touching = e->value >= 0;
			} else if (e->type == EV_KEY &&
				   e->code == BTN_TOUCH) {
				touching = e->value != 0;
			} else if (e->type == EV_SYN &&
				   e->code == SYN_REPORT) {
				if (touching && ax >= 0 && ay >= 0) {
					sx += ax;
					sy += ay;
					npts++;
				} else if (!touching && npts > 0) {
					*ox = (int)(sx / npts);
					*oy = (int)(sy / npts);
					return 0;
				}
			}
		}
	}
}

/* Solve the 3x3 normal equation M * v = r for (a,b,c) or (d,e,f). */
static int solve3(double m[3][3], double r[3], double v[3])
{
	double a[3][4];
	int i, j, k;

	for (i = 0; i < 3; i++) {
		for (j = 0; j < 3; j++)
			a[i][j] = m[i][j];
		a[i][3] = r[i];
	}
	for (k = 0; k < 3; k++) {
		int p = k;
		double t;

		for (i = k + 1; i < 3; i++)
			if (fabs(a[i][k]) > fabs(a[p][k]))
				p = i;
		if (fabs(a[p][k]) < 1e-9)
			return -1;
		for (j = k; j < 4; j++) {
			t = a[k][j];
			a[k][j] = a[p][j];
			a[p][j] = t;
		}
		for (i = k + 1; i < 3; i++) {
			double f = a[i][k] / a[k][k];
			for (j = k; j < 4; j++)
				a[i][j] -= f * a[k][j];
		}
	}
	for (i = 2; i >= 0; i--) {
		double s = a[i][3];
		for (j = i + 1; j < 3; j++)
			s -= a[i][j] * v[j];
		v[i] = s / a[i][i];
	}
	return 0;
}

/* Least-squares affine: for each output component, fit o = v0*x+v1*y+v2. */
static int fit_affine(const int rx[N_TARGETS], const int ry[N_TARGETS],
		      const int tx[N_TARGETS], const int ty[N_TARGETS],
		      double co[6])
{
	double m[3][3] = { {0} }, rxv[3] = {0}, ryv[3] = {0};
	int i, j, k;

	for (i = 0; i < N_TARGETS; i++) {
		double b[3] = { rx[i], ry[i], 1.0 };
		for (j = 0; j < 3; j++) {
			for (k = 0; k < 3; k++)
				m[j][k] += b[j] * b[k];
			rxv[j] += b[j] * tx[i];
			ryv[j] += b[j] * ty[i];
		}
	}
	if (solve3(m, rxv, co) < 0 || solve3(m, ryv, co + 3) < 0)
		return -1;
	return 0;
}

static int write_cal(const double co[6],
		     const int raw_x[N_TARGETS], const int raw_y[N_TARGETS])
{
	FILE *f = fopen(CAL_TMP, "w"), *put;
	int i, rc;

	if (!f) {
		fprintf(stderr, "touch-cal: %s: %s\n", CAL_TMP,
			strerror(errno));
		return -1;
	}
	fprintf(f, "# touch calibration (#33): fb_x = a*x+b*y+c, fb_y = d*x+e*y+f\n");
	fprintf(f, "touch_cal=%.9g,%.9g,%.9g,%.9g,%.9g,%.9g\n",
		co[0], co[1], co[2], co[3], co[4], co[5]);
	for (i = 0; i < N_TARGETS; i++)
		fprintf(f, "# point%d raw=(%d,%d)\n", i + 1,
			raw_x[i], raw_y[i]);
	fclose(f);

	/* /boot may be unmounted or read-only after boot; the boot
	 * drive's files are changed through boot-edit instead (#20). */
	put = popen(CAL_PUT " < " CAL_TMP, "w");
	if (!put) {
		fprintf(stderr, "touch-cal: boot-edit: %s\n",
			strerror(errno));
		return -1;
	}
	rc = pclose(put);
	if (rc != 0) {
		fprintf(stderr, "touch-cal: %s failed (rc=%d)\n",
			CAL_PUT, rc);
		return -1;
	}
	printf("touch-cal: saved %s on the boot drive\n", CAL_NAME);
	return 0;
}

int main(int argc, char *argv[])
{
	const char *input = INPUT_DEV;
	int infd, i;
	int targ_x[N_TARGETS], targ_y[N_TARGETS];
	int raw_x[N_TARGETS], raw_y[N_TARGETS];
	double co[6];

	for (i = 1; i < argc; i++) {
		if (strcmp(argv[i], "-h") == 0 ||
		    strcmp(argv[i], "--help") == 0) {
			fputs("usage: touch-cal [/dev/input/eventN]\n"
			      "  5-point touch calibration; writes " CAL_NAME
			      " on the boot drive\n",
			      stderr);
			return 0;
		}
		input = argv[i];
	}

	infd = open(input, O_RDONLY);
	if (infd < 0) {
		fprintf(stderr, "touch-cal: %s: %s\n", input, strerror(errno));
		return 1;
	}

	fbfd = open(FB0, O_WRONLY);
	if (fbfd < 0 ||
	    ioctl(fbfd, FBIOGET_VSCREENINFO, &var) < 0 ||
	    ioctl(fbfd, FBIOGET_FSCREENINFO, &fix) < 0) {
		fprintf(stderr, "touch-cal: fb: %s\n", strerror(errno));
		return 1;
	}
	if (var.bits_per_pixel != 16 && var.bits_per_pixel != 24 &&
	    var.bits_per_pixel != 32) {
		fprintf(stderr, "touch-cal: unsupported fb bpp %u\n",
			var.bits_per_pixel);
		return 1;
	}
	bypp = (int)((var.bits_per_pixel + 7) / 8);

	kdfd = console_fd();
	if (kdfd >= 0 && ioctl(kdfd, KDSETMODE, KD_GRAPHICS) < 0) {
		close(kdfd);
		kdfd = -1;
	}
	signal(SIGTERM, on_stop);
	signal(SIGINT, on_stop);
	signal(SIGHUP, on_stop);
	setvbuf(stdout, NULL, _IOLBF, 0);

	targ_x[0] = MARGIN;
	targ_y[0] = MARGIN;
	targ_x[1] = (int)var.xres - 1 - MARGIN;
	targ_y[1] = MARGIN;
	targ_x[2] = MARGIN;
	targ_y[2] = (int)var.yres - 1 - MARGIN;
	targ_x[3] = (int)var.xres - 1 - MARGIN;
	targ_y[3] = (int)var.yres - 1 - MARGIN;
	targ_x[4] = (int)var.xres / 2;
	targ_y[4] = (int)var.yres / 2;

	printf("touch-cal: %dx%d; tap each numbered crosshair\n",
	       (int)var.xres, (int)var.yres);

	for (i = 0; i < N_TARGETS && !stopped; i++) {
		draw_target(i, targ_x[i], targ_y[i]);
		printf("point %d: tap (%d,%d)\n", i + 1,
		       targ_x[i], targ_y[i]);
		if (read_tap(infd, &raw_x[i], &raw_y[i]) < 0)
			break;
		printf("  raw (%d,%d)\n", raw_x[i], raw_y[i]);
	}

	if (i < N_TARGETS) {
		fprintf(stderr, "touch-cal: aborted at point %d\n", i + 1);
		goto out;
	}
	if (fit_affine(raw_x, raw_y, targ_x, targ_y, co) < 0) {
		fprintf(stderr, "touch-cal: singular point set\n");
		goto out;
	}

	printf("touch_cal=%.9g,%.9g,%.9g,%.9g,%.9g,%.9g\n",
	       co[0], co[1], co[2], co[3], co[4], co[5]);
	for (i = 0; i < N_TARGETS; i++) {
		double ex = co[0] * raw_x[i] + co[1] * raw_y[i] + co[2]
			    - targ_x[i];
		double ey = co[3] * raw_x[i] + co[4] * raw_y[i] + co[5]
			    - targ_y[i];
		printf("point %d: err %.1f px (dx %.1f dy %.1f)\n", i + 1,
		       sqrt(ex * ex + ey * ey), ex, ey);
	}
	if (write_cal(co, raw_x, raw_y) == 0)
		printf("wrote %s\n", CAL_NAME);

	fill_fb(0);
out:
	if (kdfd >= 0) {
		kd_text(kdfd);
		close(kdfd);
	}
	close(fbfd);
	close(infd);
	return 0;
}
