// SPDX-License-Identifier: GPL-2.0-only
/*
 * wifi-qr - show the SoftAP credentials as a QR code.
 *
 *   wifi-qr                 QR as UTF-8 half-block text on stdout
 *                           (use `wifi-qr > /dev/tty0` for the panel console)
 *   wifi-qr --ascii         same but "##"/"  " glyphs for dumb terminals
 *   wifi-qr --panel         QR drawn onto /dev/fb0 (Tab5 720x1280 panel)
 *   wifi-qr --clear         clear the QR via /dev/tty0
 *   wifi-qr --for SECONDS   show for N seconds, then clear itself;
 *                           with --panel the console is frozen meanwhile
 *
 * Reads /tmp/credentials.txt (root-only): ssid= and psk=.
 *
 * NOMMU: no fork, no malloc; everything lives in static buffers and the
 * flat binary stays well under 40 KB.
 */
#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>
#include <linux/fb.h>
#include <linux/kd.h>

#include "wqr.h"
#include "qrcodegen.h"

#define CRED_FILE "/tmp/credentials.txt"
#define TTY0      "/dev/tty0"
#define TTYCON    "/dev/console"
#define FB0       "/dev/fb0"

static uint8_t qr_tmp[WQR_BUF_LEN];
static uint8_t qr_buf[WQR_BUF_LEN];
static char payload[320];
static char ssid[40];
static char psk[80];

/* Longest display line we paint: 1280 px * 4 B (32 bpp upper bound). */
static uint8_t fb_line[1280 * 4];

static volatile sig_atomic_t stopped;

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

/* --clear: back to text mode if frozen, then wipe what is on screen. */
static void clear_screen(void)
{
	int fd = console_fd();

	if (fd < 0)
		return;
	kd_text(fd);
	{
		ssize_t w = write(fd, "\033[2J\033[H", 7);
		(void)w;
	}
	close(fd);
}

/* ------------------------------------------------------------------ */

/* Emit one cell of two vertically stacked modules.
 * Light modules are painted white (filled glyph), dark stay terminal
 * black -- a normal-polarity QR on a dark terminal. */
static void put_pair(int top_light, int bot_light)
{
	const char *g;

	if (top_light && bot_light)
		g = "\342\226\210";	/* U+2588 full block */
	else if (top_light)
		g = "\342\226\200";	/* U+2580 upper half */
	else if (bot_light)
		g = "\342\226\204";	/* U+2584 lower half */
	else
		g = " ";
	fputs(g, stdout);
}

static int module_dark(const uint8_t *qr, int size, int mx, int my)
{
	if (mx < 0 || my < 0 || mx >= size || my >= size)
		return 0;	/* quiet zone is light */
	return qrcodegen_getModule(qr, mx, my) ? 1 : 0;
}

static void render_utf8(const uint8_t *qr, int size)
{
	int cols = size + 2 * WQR_QUIET;
	int gr, c;

	for (gr = 0; gr < size + 2 * WQR_QUIET; gr += 2) {
		for (c = 0; c < cols; c++) {
			int top = !module_dark(qr, size, c - WQR_QUIET,
					       gr - WQR_QUIET);
			int bot = !module_dark(qr, size, c - WQR_QUIET,
					       gr + 1 - WQR_QUIET);
			put_pair(top, bot);
		}
		putchar('\n');
	}
}

static void render_ascii(const uint8_t *qr, int size)
{
	int cols = size + 2 * WQR_QUIET;
	int r, c;

	for (r = 0; r < size + 2 * WQR_QUIET; r++) {
		for (c = 0; c < cols; c++)
			fputs(module_dark(qr, size, c - WQR_QUIET,
					  r - WQR_QUIET) ? "##" : "  ", stdout);
		putchar('\n');
	}
}

/* ------------------------------------------------------------------ */

/* The scan-out GDMA does not snoop the dcache: kernel-side simplefb
 * ops write back the whole smem range after every cfb draw (kernel
 * patch 0094).  One space through fbcon triggers exactly that flush
 * for our pwrite()s.  The cursor parks in the corner, off the QR. */
static void fbcon_flush(void)
{
	int fd = console_fd();
	ssize_t w;

	if (fd < 0)
		return;
	w = write(fd, "\033[999;999H ", 10);
	(void)w;
	close(fd);
}

/* Pixels are black or white only. */
static void fill_px(uint8_t *p, int bpp, int white)
{
	uint8_t v = white ? 0xff : 0;

	p[0] = v;
	p[1] = v;
	if (bpp >= 24) {
		p[2] = v;
		if (bpp == 32)
			p[3] = 0;
	}
}

static int render_panel(const uint8_t *qr, int size)
{
	struct fb_var_screeninfo var;
	struct fb_fix_screeninfo fix;
	int fd, total, scale, ox, oy, bypp;
	unsigned int y;

	fd = open(FB0, O_WRONLY);
	if (fd < 0) {
		fprintf(stderr, "wifi-qr: %s: %s\n", FB0, strerror(errno));
		return -1;
	}
	if (ioctl(fd, FBIOGET_VSCREENINFO, &var) < 0 ||
	    ioctl(fd, FBIOGET_FSCREENINFO, &fix) < 0) {
		fprintf(stderr, "wifi-qr: fb ioctls: %s\n", strerror(errno));
		close(fd);
		return -1;
	}
	if (var.bits_per_pixel != 16 && var.bits_per_pixel != 24 &&
	    var.bits_per_pixel != 32) {
		fprintf(stderr, "wifi-qr: unsupported fb bpp %u\n",
			var.bits_per_pixel);
		close(fd);
		return -1;
	}
	bypp = (int)((var.bits_per_pixel + 7) / 8);
	if ((int)var.xres * bypp > (int)sizeof(fb_line)) {
		fprintf(stderr, "wifi-qr: fb %u px too wide\n", var.xres);
		close(fd);
		return -1;
	}

	total = size + 2 * WQR_QUIET;
	scale = (int)var.xres / total;
	if ((int)var.yres / total < scale)
		scale = (int)var.yres / total;
	if (scale < 1)
		scale = 1;
	ox = ((int)var.xres - total * scale) / 2;
	oy = ((int)var.yres - total * scale) / 2;

	for (y = 0; y < var.yres; y++) {
		int ry = (int)y - oy;
		uint8_t *p = fb_line;
		int i;

		if (ry < 0 || ry >= total * scale) {
			for (i = 0; i < (int)var.xres; i++) {
				fill_px(p, (int)var.bits_per_pixel, 0);
				p += bypp;
			}
		} else {
			int my = ry / scale - WQR_QUIET;
			int mx0 = ox;
			int mx1 = ox + total * scale;

			for (i = 0; i < (int)var.xres; i++) {
				int white = 0;

				if (i >= mx0 && i < mx1) {
					int mx = (i - mx0) / scale - WQR_QUIET;

					white = !module_dark(qr, size, mx, my);
				}
				fill_px(p, (int)var.bits_per_pixel, white);
				p += bypp;
			}
		}
		if (pwrite(fd, fb_line, (size_t)var.xres * (size_t)bypp,
			   (off_t)y * (off_t)fix.line_length) < 0) {
			fprintf(stderr, "wifi-qr: fb write: %s\n",
				strerror(errno));
			close(fd);
			return -1;
		}
	}
	close(fd);
	fbcon_flush();
	return 0;
}

/* ------------------------------------------------------------------ */

static void usage(void)
{
	fputs("usage: wifi-qr [--panel|--ascii] [--clear] [--for SECONDS]\n"
	      "  shows " CRED_FILE " ssid=/psk= as a QR code\n"
	      "  default output is a half-block text QR on stdout;\n"
	      "  `wifi-qr > /dev/tty0` puts it on the panel console\n", stderr);
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

static void hold_for(long hold)
{
	while (hold > 0 && !stopped)
		hold = (long)sleep((unsigned int)hold);
}

int main(int argc, char *argv[])
{
	int want_panel = 0, want_ascii = 0, want_clear = 0;
	long hold = -1;
	int i, size;

	for (i = 1; i < argc; i++) {
		if (strcmp(argv[i], "--panel") == 0)
			want_panel = 1;
		else if (strcmp(argv[i], "--ascii") == 0)
			want_ascii = 1;
		else if (strcmp(argv[i], "--clear") == 0)
			want_clear = 1;
		else if (strcmp(argv[i], "--for") == 0 && i + 1 < argc) {
			hold = strtol(argv[++i], NULL, 10);
			if (hold < 0)
				hold = 0;
		} else if (strcmp(argv[i], "-h") == 0 ||
			   strcmp(argv[i], "--help") == 0) {
			usage();
			return 0;
		} else {
			usage();
			return 2;
		}
	}

	if (want_clear) {
		clear_screen();
		return 0;
	}

	if (wqr_read_creds(CRED_FILE, ssid, sizeof(ssid), psk, sizeof(psk)) < 0) {
		fprintf(stderr, "wifi-qr: no ssid=/psk= in %s\n", CRED_FILE);
		return 1;
	}
	if (wqr_payload(payload, sizeof(payload), ssid, psk) < 0) {
		fprintf(stderr, "wifi-qr: credentials too long\n");
		return 1;
	}
	size = wqr_encode(payload, qr_tmp, qr_buf);
	if (size < 0) {
		fprintf(stderr, "wifi-qr: payload too long for v%d\n",
			WQR_MAX_VERSION);
		return 1;
	}

	if (hold >= 0)
		install_stop_handlers();

	if (want_panel) {
		int kdfd = -1;

		if (render_panel(qr_buf, size) < 0)
			return 1;
		if (hold >= 0) {
			kdfd = console_fd();
			if (kdfd >= 0 && ioctl(kdfd, KDSETMODE, KD_GRAPHICS) < 0) {
				close(kdfd);
				kdfd = -1;
			}
			hold_for(hold);
			if (kdfd >= 0) {
				kd_text(kdfd);	/* repaints the console */
				close(kdfd);
			} else {
				clear_screen();
			}
		}
		/* Without --for the QR simply stays until the console
		 * paints over it; `wifi-qr --clear` erases it. */
	} else {
		if (want_ascii)
			render_ascii(qr_buf, size);
		else
			render_utf8(qr_buf, size);
		printf("ssid=%s\npsk=%s\n", ssid, psk);
		fflush(stdout);
		if (hold >= 0) {
			hold_for(hold);
			fputs("\033[2J\033[H", stdout);
			fflush(stdout);
		}
	}
	return 0;
}
