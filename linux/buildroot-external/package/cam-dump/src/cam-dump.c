// SPDX-License-Identifier: GPL-2.0
/*
 * cam-dump — one-shot raw-frame capture for the Tab5 camera.
 *
 * Drives /dev/cam0 (esp32p4-csi-cam, kernel patch 0140):
 *   ioctl(CAM_PREPARE) -> SCCB init over /dev/i2c-0 -> read() one
 *   1280x720 RAW8 frame -> write to file + print stats.
 *
 * SCCB is a 16-bit-register-address I2C protocol on the internal
 * bus; the sensor answers at 0x36 after CAM_RST is raised by
 * CAM_PREPARE.
 *
 * Usage: cam-dump [-o out.raw] [-n frames] [-p] [-h]
 *   -o  output file (default /tmp/cam0.raw); '-' = stdout
 *   -n  frames to capture (default 1; each is a full cold capture)
 *   -p  enable the sensor's test pattern (0x4501 bit3) before streaming
 */

#include <errno.h>
#include <fcntl.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <unistd.h>

#define CAM_IOC_PREPARE		_IO('C', 1)
#define CAM_FRAME_BYTES		(1280 * 720)
#define SENSOR_ADDR		0x36
#define SENSOR_PID		0xeb52

/* Minimal i2c-dev ioctl plumbing (musl has no linux/i2c-dev.h here).
 * The board's i2c-0 is i2c-gpio bit-banged: combined I2C_RDWR
 * transactions fail on it (verified on board), so SCCB goes through
 * plain I2C_SMBUS ioctls -- the same path i2cset/i2cget use:
 *   write: command=reg_hi + word=(reg_lo | val<<8)  -> [hi][lo][val]
 *   read:  BYTE_DATA command=reg_hi, data=reg_lo     -> [hi][lo]
 *          then SMBUS_BYTE read                       -> [val]
 */
#define I2C_SLAVE		0x0703
#define I2C_SMBUS		0x0720
#define I2C_SMBUS_READ		1
#define I2C_SMBUS_WRITE		0
#define I2C_SMBUS_BYTE		1
#define I2C_SMBUS_BYTE_DATA	2
#define I2C_SMBUS_WORD_DATA	3
union i2c_smbus_data_local {
	uint8_t byte;
	uint16_t word;
	uint8_t block[34];
};
struct i2c_smbus_ioctl_data_local {
	uint8_t read_write;
	uint8_t command;
	uint32_t size;
	union i2c_smbus_data_local *data;
};

/* cam0 is powered only while open: the driver drops CAM_RST on
 * release, so every capture iteration re-does PREPARE + the SCCB
 * table + stream-on.  The sensor loses its registers on power
 * down; nothing persists between frames but the i2c-0 fd. */

/* Published SC202CS 1-lane 24 MHz-input 1280x720 RAW8 table --
 * the same 131 writes the CAMERA_ORACLE shim used (0xffff=end,
 * 0xfffe=ms delay). */
static const uint16_t sensor_init[][2] = {
	{0x0103, 0x01}, {0x0100, 0x00}, {0x36e9, 0x80}, {0x36ea, 0x06},
	{0x36eb, 0x0a}, {0x36ec, 0x01}, {0x36ed, 0x18}, {0x36e9, 0x24},
	{0x301f, 0x18}, {0x3031, 0x08}, {0x3037, 0x00}, {0x3200, 0x00},
	{0x3201, 0xa0}, {0x3202, 0x00}, {0x3203, 0xf0}, {0x3204, 0x05},
	{0x3205, 0xa7}, {0x3206, 0x03}, {0x3207, 0xc7}, {0x3208, 0x05},
	{0x3209, 0x00}, {0x320a, 0x02}, {0x320b, 0xd0}, {0x3210, 0x00},
	{0x3211, 0x04}, {0x3212, 0x00}, {0x3213, 0x04}, {0x3301, 0xff},
	{0x3304, 0x68}, {0x3306, 0x40}, {0x3308, 0x08}, {0x3309, 0xa8},
	{0x330b, 0xd0}, {0x330c, 0x18}, {0x330d, 0xff}, {0x330e, 0x20},
	{0x331e, 0x59}, {0x331f, 0x99}, {0x3333, 0x10}, {0x335e, 0x06},
	{0x335f, 0x08}, {0x3364, 0x1f}, {0x337c, 0x02}, {0x337d, 0x0a},
	{0x338f, 0xa0}, {0x3390, 0x01}, {0x3391, 0x03}, {0x3392, 0x1f},
	{0x3393, 0xff}, {0x3394, 0xff}, {0x3395, 0xff}, {0x33a2, 0x04},
	{0x33ad, 0x0c}, {0x33b1, 0x20}, {0x33b3, 0x38}, {0x33f9, 0x40},
	{0x33fb, 0x48}, {0x33fc, 0x0f}, {0x33fd, 0x1f}, {0x349f, 0x03},
	{0x34a6, 0x03}, {0x34a7, 0x1f}, {0x34a8, 0x38}, {0x34a9, 0x30},
	{0x34ab, 0xd0}, {0x34ad, 0xd8}, {0x34f8, 0x1f}, {0x34f9, 0x20},
	{0x3630, 0xa0}, {0x3631, 0x92}, {0x3632, 0x64}, {0x3633, 0x43},
	{0x3637, 0x49}, {0x363a, 0x85}, {0x363c, 0x0f}, {0x3650, 0x31},
	{0x3670, 0x0d}, {0x3674, 0xc0}, {0x3675, 0xa0}, {0x3676, 0xa0},
	{0x3677, 0x92}, {0x3678, 0x96}, {0x3679, 0x9a}, {0x367c, 0x03},
	{0x367d, 0x0f}, {0x367e, 0x01}, {0x367f, 0x0f}, {0x3698, 0x83},
	{0x3699, 0x86}, {0x369a, 0x8c}, {0x369b, 0x94}, {0x36a2, 0x01},
	{0x36a3, 0x03}, {0x36a4, 0x07}, {0x36ae, 0x0f}, {0x36af, 0x1f},
	{0x36bd, 0x22}, {0x36be, 0x22}, {0x36bf, 0x22}, {0x36d0, 0x01},
	{0x370f, 0x02}, {0x3721, 0x6c}, {0x3722, 0x8d}, {0x3725, 0xc5},
	{0x3727, 0x14}, {0x3728, 0x04}, {0x37b7, 0x04}, {0x37b8, 0x04},
	{0x37b9, 0x06}, {0x37bd, 0x07}, {0x37be, 0x0f}, {0x3901, 0x02},
	{0x3903, 0x40}, {0x3905, 0x8d}, {0x3907, 0x00}, {0x3908, 0x41},
	{0x391f, 0x41}, {0x3933, 0x80}, {0x3934, 0x02}, {0x3937, 0x6f},
	{0x393a, 0x01}, {0x393d, 0x01}, {0x393e, 0xc0}, {0x39dd, 0x41},
	{0x3e00, 0x00}, {0x3e01, 0x3d}, {0x3e02, 0xc0}, {0x3e09, 0x00},
	{0x4509, 0x28}, {0x450d, 0x61},
	{0xffff, 0x00},
};

static int sccb_write(int fd, uint16_t reg, uint8_t val)
{
	union i2c_smbus_data_local d;
	struct i2c_smbus_ioctl_data_local a = {
		.read_write = I2C_SMBUS_WRITE,
		.command = (uint8_t)(reg >> 8),
		.size = I2C_SMBUS_WORD_DATA,
		.data = &d,
	};
	/* wire order is command, word low byte, word high byte:
	 * [reg_hi][reg_lo][val] -- the 16-bit-reg SCCB write. */
	d.word = (uint16_t)((reg & 0xff) | (val << 8));
	return ioctl(fd, I2C_SMBUS, &a);
}

static int sccb_read(int fd, uint16_t reg, uint8_t *val)
{
	union i2c_smbus_data_local d;
	struct i2c_smbus_ioctl_data_local a = {
		.read_write = I2C_SMBUS_WRITE,
		.command = (uint8_t)(reg >> 8),
		.size = I2C_SMBUS_BYTE_DATA,
		.data = &d,
	};
	*val = 0;
	/* pointer phase: [reg_hi][reg_lo], then a bare byte read. */
	d.byte = reg & 0xff;
	if (ioctl(fd, I2C_SMBUS, &a))
		return -1;
	memset(&a, 0, sizeof a);
	a.read_write = I2C_SMBUS_READ;
	a.size = I2C_SMBUS_BYTE;
	a.data = &d;
	if (ioctl(fd, I2C_SMBUS, &a))
		return -1;
	*val = d.byte;
	return 0;
}

static void stats(const uint8_t *f)
{
	unsigned long sum = 0, sum16 = 0;
	int mn = 255, mx = 0, i, n16 = 0;

	for (i = 0; i < CAM_FRAME_BYTES; i++) {
		sum += f[i];
		if (f[i] < mn) mn = f[i];
		if (f[i] > mx) mx = f[i];
	}
	/* leftmost 16-byte stripe mean: pattern ramps show ~2->131,
	 * a dead pipeline leaves the 0xa5 fill or reads black. */
	for (i = 0; i < 720; i++) {
		int x;
		for (x = 0; x < 16; x++)
			sum16 += f[i * 1280 + x], n16++;
	}
	fprintf(stderr, "frame: mean=%lu min=%d max=%d stripe16mean=%lu\n",
		sum / CAM_FRAME_BYTES, mn, mx, sum16 / n16);
}

/* Power up + program the sensor for one capture.  Call after
 * CAM_PREPARE armed the receiver; the close() at frame end drops
 * CAM_RST, so this runs again for the next frame. */
static int sensor_setup(int i2c, int pattern)
{
	uint8_t idh, idl;
	int i;

	for (i = 0; i < 5; i++) {
		if (!sccb_read(i2c, 0x3107, &idh) &&
		    !sccb_read(i2c, 0x3108, &idl))
			break;
		usleep(20000);
	}
	if (i == 5) {
		fprintf(stderr, "chip-ID read failed (sensor powered?)\n");
		return -1;
	}
	if ((idh << 8 | idl) != SENSOR_PID) {
		fprintf(stderr, "chip ID 0x%02x%02x, want %04x\n",
			idh, idl, SENSOR_PID);
		return -1;
	}
	for (i = 0; sensor_init[i][0] != 0xffff; i++) {
		if (sensor_init[i][0] == 0xfffe) {
			usleep(sensor_init[i][1] * 1000);
			continue;
		}
		if (sccb_write(i2c, sensor_init[i][0], sensor_init[i][1])) {
			fprintf(stderr, "init write 0x%04x failed\n",
				sensor_init[i][0]);
			return -1;
		}
	}
	if (pattern && sccb_write(i2c, 0x4501, 0x08)) {
		fprintf(stderr, "test-pattern write failed\n");
		return -1;
	}
	if (sccb_write(i2c, 0x0100, 0x01)) {
		fprintf(stderr, "stream-on write failed\n");
		return -1;
	}
	usleep(50000);	/* sensor ramp to first frame */
	return 0;
}

int main(int argc, char **argv)
{
	const char *out = "/tmp/cam0.raw";
	int frames = 1, pattern = 0, i2c = -1, ofd = -1, rc = 1;
	uint8_t *fb;
	int i;

	for (i = 1; i < argc; i++) {
		if (!strcmp(argv[i], "-o") && i + 1 < argc)
			out = argv[++i];
		else if (!strcmp(argv[i], "-n") && i + 1 < argc)
			frames = atoi(argv[++i]);
		else if (!strcmp(argv[i], "-p"))
			pattern = 1;
		else {
			fprintf(stderr,
				"usage: cam-dump [-o out.raw|-] [-n frames] [-p]\n");
			return 2;
		}
	}
	if (frames < 1)
		frames = 1;

	i2c = open("/dev/i2c-0", O_RDWR);
	if (i2c < 0) {
		perror("open /dev/i2c-0");
		return 1;
	}
	if (ioctl(i2c, I2C_SLAVE, SENSOR_ADDR) < 0) {
		perror("I2C_SLAVE");
		return 1;
	}
	if (!strcmp(out, "-"))
		ofd = STDOUT_FILENO;
	else
		ofd = open(out, O_WRONLY | O_CREAT | O_TRUNC, 0666);
	if (ofd < 0) {
		perror("open output");
		goto out;
	}
	fb = malloc(CAM_FRAME_BYTES);
	if (!fb) {
		fprintf(stderr, "malloc failed\n");
		goto out;
	}
	for (i = 0; i < frames; i++) {
		int cam;
		ssize_t got = 0, r;
		size_t off = 0;

		/* open -> PREPARE powers the rail and arms the pipe;
		 * close disarms and drops CAM_RST, so every frame is a
		 * full cold capture: PREPARE + SCCB table + stream-on. */
		cam = open("/dev/cam0", O_RDONLY);
		if (cam < 0) {
			perror("open /dev/cam0");
			goto out;
		}
		if (ioctl(cam, CAM_IOC_PREPARE, 0) < 0) {
			perror("CAM_PREPARE");
			close(cam);
			goto out;
		}
		if (sensor_setup(i2c, pattern)) {
			close(cam);
			goto out;
		}
		while ((r = read(cam, fb + off, CAM_FRAME_BYTES - off)) > 0)
			off += r;
		got = off;
		close(cam);
		if (r < 0) {
			perror("read /dev/cam0");
			goto out;
		}
		fprintf(stderr, "frame %d: %zd bytes\n", i + 1, got);
		if (got != CAM_FRAME_BYTES) {
			fprintf(stderr, "short frame\n");
			goto out;
		}
		stats(fb);
		if (i == frames - 1 && write(ofd, fb, got) != got) {
			perror("write output");
			goto out;
		}
	}
	rc = 0;
out:
	if (ofd >= 0 && ofd != STDOUT_FILENO)
		close(ofd);
	if (i2c >= 0)
		close(i2c);
	return rc;
}
