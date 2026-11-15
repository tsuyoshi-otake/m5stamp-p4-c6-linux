// SPDX-License-Identifier: GPL-2.0-only
/*
 * fb-mmap-test: draw through an mmap of /dev/fb0 only, to prove the
 * simplefb espressif,flush-cache mmap writeback path (#15).
 *
 * Build for the Tab5 rootfs (bFLT; same flags as #17 documents):
 *
 *   riscv32-buildroot-linux-uclibc-gcc -O2 -fPIC -Wl,-elf2flt=-r -static \
 *       -o fb-mmap-test linux/tests/fb-mmap-test.c
 *
 * (The bFLT floor is BusyBox's 16000-byte stack; nothing here needs
 * more, so no flthdr -s step is required.)
 *
 * What it does:
 *   1. mmaps /dev/fb0 and paints three colour bands through the
 *      mapping only -- no write(), no ioctl.  On the buggy kernel the
 *      bands reach the panel only as evictions happen to publish them;
 *      with espressif,flush-interval-ms they appear within one tick.
 *   2. Calls FBIOPAN_DISPLAY and FBIO_WAITFORVSYNC, the "frame done"
 *      hints, then paints a fourth band; both must publish it at once.
 *   3. Sleeps so the mmap flag stays visible in
 *      /proc/<pid>/maps, then exits; the periodic writeback must stop
 *      once the last user closes.
 *
 * Watch on the device: the bands must appear complete, not in pieces.
 *
 * One black character cell in the picture is not a fault.  The text
 * console still owns the panel and repaints the cell under its cursor
 * every time the cursor blinks.  Hide the cursor first to get a clean
 * picture, and show it again afterwards:
 *
 *   printf '\033[?25l' > /dev/tty1
 *   printf '\033[?25h' > /dev/tty1
 */

#include <errno.h>
#include <fcntl.h>
#include <linux/fb.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <unistd.h>

#ifndef FBIO_WAITFORVSYNC
#define FBIO_WAITFORVSYNC _IOW('F', 0x20, __u32)
#endif

static void fill_band(uint16_t *fb, unsigned int stride_px,
		      unsigned int y0, unsigned int y1, uint16_t rgb565)
{
	unsigned int x, y;

	for (y = y0; y < y1; y++)
		for (x = 0; x < stride_px; x++)
			fb[y * stride_px + x] = rgb565;
}

int main(void)
{
	struct fb_fix_screeninfo fix;
	struct fb_var_screeninfo var;
	uint16_t *fb;
	unsigned int h, band;
	int fd, i;

	fd = open("/dev/fb0", O_RDWR);
	if (fd < 0) {
		perror("open /dev/fb0");
		return 1;
	}
	if (ioctl(fd, FBIOGET_FSCREENINFO, &fix) < 0 ||
	    ioctl(fd, FBIOGET_VSCREENINFO, &var) < 0) {
		perror("FBIOGET_*SCREENINFO");
		return 1;
	}
	printf("fb0: %ux%u stride %u len %u\n", var.xres, var.yres,
	       fix.line_length, fix.smem_len);

	fb = mmap(NULL, fix.smem_len, PROT_READ | PROT_WRITE, MAP_SHARED,
		  fd, 0);
	if (fb == MAP_FAILED) {
		perror("mmap");
		return 1;
	}

	/* Four bands top to bottom; the last one rides on the ioctls. */
	band = var.yres / 4;
	h = fix.line_length / sizeof(*fb);
	fill_band(fb, h, 0, band, 0xf800);          /* red */
	fill_band(fb, h, band, 2 * band, 0x07e0);   /* green */
	fill_band(fb, h, 2 * band, 3 * band, 0x001f); /* blue */
	printf("painted 3 bands via mmap only\n");

	/* "Frame done" hints: each must publish the pending pixels. */
	if (ioctl(fd, FBIOPAN_DISPLAY, &var) < 0)
		printf("FBIOPAN_DISPLAY: %s\n", strerror(errno));
	i = 0;
	if (ioctl(fd, FBIO_WAITFORVSYNC, &i) < 0)
		printf("FBIO_WAITFORVSYNC: %s\n", strerror(errno));
	fill_band(fb, h, 3 * band, var.yres, 0xffff); /* white */
	printf("painted 4th band after pan/waitforvsync\n");

	sleep(3);
	munmap(fb, fix.smem_len);
	close(fd);
	printf("done; periodic writeback should now be stopped\n");
	return 0;
}
