/*
 * touch-dots: draw a white dot where the Tab5 panel is touched (#33).
 *
 * Reads evdev events (default /dev/input/event0) and pwrite()s a small
 * square into /dev/fb0 at the touched position, like a connect-the-dots
 * demo.  Both the ST712x touchscreen path (BTN_TOUCH + ABS_X/ABS_Y) and
 * the MT path (ABS_MT_POSITION_*) feed the same coordinates; a dot lands
 * on each SYN_REPORT while a contact is down.
 *
 * The scan-out GDMA does not snoop the dcache, so after each dot an
 * FBIOPAN_DISPLAY ioctl triggers the smem write-back kernel patch 0094
 * added (the same path fbcon uses).  Dots accumulate until Ctrl-C.
 *
 * Cross-builds as part of the touch-dots Buildroot package (Tab5 lab
 * image).  Host-buildable for tests: gcc -o touch-dots src/touch-dots.c
 */

#include <errno.h>
#include <fcntl.h>
#include <linux/fb.h>
#include <linux/input.h>
#include <linux/kd.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <unistd.h>

#define INPUT_DEV "/dev/input/event0"
#define FB0 "/dev/fb0"
#define TTY0 "/dev/tty0"
#define TTYCON "/dev/console"

/* Dot is a filled square, DOT_R px to each side of the touch point. */
#define DOT_R 4

static struct fb_var_screeninfo var;
static struct fb_fix_screeninfo fix;
static int bypp;
static int fbfd = -1;
static int kdfd = -1;
static volatile sig_atomic_t stopped;

static void on_stop(int sig)
{
	(void)sig;
	stopped = 1;
}

static void install_stop_handlers(void)
{
	struct sigaction sa;

	memset(&sa, 0, sizeof(sa));
	sa.sa_handler = on_stop;
	sigaction(SIGTERM, &sa, NULL);
	sigaction(SIGINT, &sa, NULL);
	sigaction(SIGHUP, &sa, NULL);
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

static void fill_px(uint8_t *p, int white)
{
	p[0] = p[1] = p[2] = white ? 0xff : 0;
	if (var.bits_per_pixel == 32)
		p[3] = 0;
	else if (var.bits_per_pixel == 16) {
		/* RGB565 white is 0xffff; black is 0. */
		p[0] = p[1] = white ? 0xff : 0;
	}
}

static void put_dot(int cx, int cy)
{
	uint8_t px[4];
	int x, y;

	fill_px(px, 1);
	for (y = cy - DOT_R; y <= cy + DOT_R; y++) {
		if (y < 0 || y >= (int)var.yres)
			continue;
		for (x = cx - DOT_R; x <= cx + DOT_R; x++) {
			off_t off;

			if (x < 0 || x >= (int)var.xres)
				continue;
			off = (off_t)y * fix.line_length +
			      (off_t)x * bypp;
			if (pwrite(fbfd, px, bypp, off) < 0) {
				fprintf(stderr, "touch-dots: fb write: %s\n",
					strerror(errno));
				return;
			}
		}
	}
	/* Trigger the 0094 write-back so the dot reaches the panel. */
	ioctl(fbfd, FBIOPAN_DISPLAY, &var);
	printf("%d %d\n", cx, cy);
	fflush(stdout);
}

int main(int argc, char *argv[])
{
	const char *input = INPUT_DEV;
	int infd, max_x = -1, max_y = -1;
	int ax = -1, ay = -1, touching = 0;
	struct input_absinfo info;
	struct input_event evs[16];
	ssize_t n;
	int i;

	for (i = 1; i < argc; i++) {
		if (strcmp(argv[i], "-h") == 0 ||
		    strcmp(argv[i], "--help") == 0) {
			fputs("usage: touch-dots [/dev/input/eventN]\n"
			      "  draws a white dot on /dev/fb0 where the panel\n"
			      "  is touched; Ctrl-C restores the text console\n",
			      stderr);
			return 0;
		}
		input = argv[i];
	}

	infd = open(input, O_RDONLY);
	if (infd < 0) {
		fprintf(stderr, "touch-dots: %s: %s\n", input,
			strerror(errno));
		return 1;
	}
	if (ioctl(infd, EVIOCGABS(ABS_X), &info) == 0)
		max_x = info.maximum;
	if (ioctl(infd, EVIOCGABS(ABS_Y), &info) == 0)
		max_y = info.maximum;

	fbfd = open(FB0, O_WRONLY);
	if (fbfd < 0) {
		fprintf(stderr, "touch-dots: %s: %s\n", FB0,
			strerror(errno));
		close(infd);
		return 1;
	}
	if (ioctl(fbfd, FBIOGET_VSCREENINFO, &var) < 0 ||
	    ioctl(fbfd, FBIOGET_FSCREENINFO, &fix) < 0) {
		fprintf(stderr, "touch-dots: fb ioctls: %s\n",
			strerror(errno));
		close(fbfd);
		close(infd);
		return 1;
	}
	if (var.bits_per_pixel != 16 && var.bits_per_pixel != 24 &&
	    var.bits_per_pixel != 32) {
		fprintf(stderr, "touch-dots: unsupported fb bpp %u\n",
			var.bits_per_pixel);
		close(fbfd);
		close(infd);
		return 1;
	}
	bypp = (int)((var.bits_per_pixel + 7) / 8);
	if (max_x <= 0)
		max_x = (int)var.xres - 1;
	if (max_y <= 0)
		max_y = (int)var.yres - 1;

	/* Hide fbcon so boot text does not paint over the dots; restored
	 * on exit. */
	kdfd = console_fd();
	if (kdfd >= 0 && ioctl(kdfd, KDSETMODE, KD_GRAPHICS) < 0) {
		close(kdfd);
		kdfd = -1;
	}
	install_stop_handlers();
	setvbuf(stdout, NULL, _IOLBF, 0);
	printf("touch-dots: %dx%d on %s (abs max %dx%d%s); touch the panel\n",
	       (int)var.xres, (int)var.yres, input, max_x, max_y, "");
	fflush(stdout);

	while (!stopped && (n = read(infd, evs, sizeof(evs))) > 0) {
		int off = 0;

		for (off = 0; off + (int)sizeof(struct input_event) <= n;
		     off += (int)sizeof(struct input_event)) {
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
				if (e->code == ABS_MT_POSITION_X ||
				    e->code == ABS_MT_POSITION_Y ||
				    e->code == ABS_MT_TRACKING_ID)
					touching = e->value >= 0;
			} else if (e->type == EV_KEY &&
				   e->code == BTN_TOUCH) {
				touching = e->value != 0;
			} else if (e->type == EV_SYN &&
				   e->code == SYN_REPORT &&
				   touching && ax >= 0 && ay >= 0) {
				put_dot(ax * ((int)var.xres - 1) / max_x,
					ay * ((int)var.yres - 1) / max_y);
			}
		}
	}

	if (kdfd >= 0) {
		kd_text(kdfd);
		close(kdfd);
	}
	close(fbfd);
	close(infd);
	return 0;
}
