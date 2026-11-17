/* Minimal NOMMU aplay-file: stream a raw S16_LE stereo 48 kHz file to
 * the first playback PCM -- exactly what pcm-rec writes.  alsa-utils
 * needs fork() (BR2_USE_MMU) and tinyalsa needs dynamic linking, so
 * this speaks the raw SNDRV_PCM ioctl ABI only -- no libasound (#24).
 *
 * Usage: pcm-play [card] file [-g dB]
 *   pcm-play /tmp/r.pcm       -> play file on pcmC0D0p
 *   pcm-play 0 /tmp/r.pcm     -> same, card given explicitly
 *   pcm-play 0 /tmp/r.pcm -g -6 -> 6 dB attenuation
 * The file is read in chunks; it is never loaded whole.
 */
#include <errno.h>
#include <fcntl.h>
#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <unistd.h>
#include <sound/asound.h>

#define MASK(p, v) ((p)->masks[(v) - SNDRV_PCM_HW_PARAM_FIRST_MASK])
#define IVL(p, v) ((p)->intervals[(v) - SNDRV_PCM_HW_PARAM_FIRST_INTERVAL])

static void mask_any(struct snd_mask *m)
{
	memset(m->bits, 0xff, sizeof(m->bits));
}

static void mask_one(struct snd_mask *m, unsigned int v)
{
	memset(m->bits, 0, sizeof(m->bits));
	m->bits[v >> 5] = 1u << (v & 31);
}

static void ivl_any(struct snd_interval *i)
{
	i->min = 0;
	i->max = (unsigned)-1;
	i->integer = 1;
}

static void ivl_set(struct snd_interval *i, unsigned int v)
{
	i->min = i->max = v;
	i->integer = 1;
}

static int all_digits(const char *s)
{
	if (!*s)
		return 0;
	for (; *s; s++)
		if (*s < '0' || *s > '9')
			return 0;
	return 1;
}

int main(int argc, char **argv)
{
	int card = 0, ai = 1;
	const char *path = NULL;
	double db = 0.0, gain = 1.0;
	unsigned int rate = 48000;
	char dev[64];
	struct snd_pcm_hw_params *p;
	struct snd_xferi xf;
	int16_t *buf;
	int fd = -1, ff = -1, err, m;
	long done = 0;

	if (ai < argc && all_digits(argv[ai]))
		card = atoi(argv[ai++]);
	if (ai < argc)
		path = argv[ai++];
	if (ai + 1 < argc && !strcmp(argv[ai], "-g"))
		db = atof(argv[ai + 1]);
	if (!path) {
		fprintf(stderr, "usage: pcm-play [card] file [-g dB]\n");
		return 1;
	}
	ff = open(path, O_RDONLY);
	if (ff < 0) {
		perror(path);
		return 1;
	}
	if (db > 0.0)
		db = 0.0;
	/* pow() crashes on this NOMMU uClibc BFLT runtime, so the
	 * dB->linear factor is a multiply loop (same as pcm-sine):
	 * exact for integer dB, <= ~0.1 dB off for fractions. */
	while (db <= -1.0) {
		gain *= 0.8912509381; /* 10^(-1/20) */
		db += 1.0;
	}
	gain *= 1.0 + db * 0.10875; /* db in (-1,0] */

	snprintf(dev, sizeof(dev), "/dev/snd/pcmC%dD0p", card);
	fd = open(dev, O_RDWR);
	if (fd < 0) {
		perror(dev);
		return 1;
	}

	p = calloc(1, sizeof(*p));
	for (m = SNDRV_PCM_HW_PARAM_FIRST_MASK;
	     m <= SNDRV_PCM_HW_PARAM_LAST_MASK; m++)
		mask_any(&MASK(p, m));
	for (m = SNDRV_PCM_HW_PARAM_FIRST_INTERVAL;
	     m <= SNDRV_PCM_HW_PARAM_LAST_INTERVAL; m++)
		ivl_any(&IVL(p, m));
	p->rmask = ~0U;
	p->cmask = 0;

	err = ioctl(fd, SNDRV_PCM_IOCTL_HW_REFINE, p);
	if (err < 0) {
		perror("HW_REFINE");
		return 1;
	}
	fprintf(stderr, "refined: rate [%u,%u] ch [%u,%u]\n",
		IVL(p, SNDRV_PCM_HW_PARAM_RATE).min,
		IVL(p, SNDRV_PCM_HW_PARAM_RATE).max,
		IVL(p, SNDRV_PCM_HW_PARAM_CHANNELS).min,
		IVL(p, SNDRV_PCM_HW_PARAM_CHANNELS).max);

	/* Pin: RW_INTERLEAVED, S16_LE, stereo, 48 kHz -- identical to
	 * pcm-sine so period/buffer sizing hits the driver's rules
	 * engine the same proven way (period_time 50 ms, periods 4). */
	mask_one(&MASK(p, SNDRV_PCM_HW_PARAM_ACCESS),
		 SNDRV_PCM_ACCESS_RW_INTERLEAVED);
	mask_one(&MASK(p, SNDRV_PCM_HW_PARAM_FORMAT),
		 SNDRV_PCM_FORMAT_S16_LE);
	mask_one(&MASK(p, SNDRV_PCM_HW_PARAM_SUBFORMAT),
		 SNDRV_PCM_SUBFORMAT_STD);
	ivl_set(&IVL(p, SNDRV_PCM_HW_PARAM_CHANNELS), 2);
	ivl_set(&IVL(p, SNDRV_PCM_HW_PARAM_RATE), rate);
	ivl_set(&IVL(p, SNDRV_PCM_HW_PARAM_PERIOD_TIME), 50000);
	ivl_set(&IVL(p, SNDRV_PCM_HW_PARAM_PERIODS), 4);
	p->rmask = ~0U;
	p->cmask = 0;

	err = ioctl(fd, SNDRV_PCM_IOCTL_HW_PARAMS, p);
	if (err < 0) {
		perror("HW_PARAMS");
		return 1;
	}
	err = ioctl(fd, SNDRV_PCM_IOCTL_PREPARE, 0);
	if (err < 0) {
		perror("PREPARE");
		return 1;
	}

	buf = malloc(960 * 2 * sizeof(int16_t));
	if (!buf) {
		fprintf(stderr, "malloc failed\n");
		return 1;
	}
	for (;;) {
		static int stall;
		ssize_t n = read(ff, buf, 960 * 2 * sizeof(int16_t));
		long want, j;
		if (n < 0) {
			if (errno == EINTR)
				continue;
			perror("read");
			return 1;
		}
		if (n == 0)
			break; /* EOF */
		want = n / 4; /* whole stereo frames */
		if (gain != 1.0) {
			for (j = 0; j < want * 2; j++) {
				long v = lrint(buf[j] * gain);
				buf[j] = v > 32767 ? 32767 :
					 v < -32768 ? -32768 : v;
			}
		}
		long off = 0;
		while (off < want) {
			xf.buf = buf + off * 2;
			xf.frames = want - off;
			xf.result = 0;
			err = ioctl(fd, SNDRV_PCM_IOCTL_WRITEI_FRAMES, &xf);
			if (err < 0) {
				if (errno == EAGAIN || errno == EINTR)
					continue;
				perror("WRITEI_FRAMES");
				return 1;
			}
			if (xf.result <= 0) {
				if (++stall > 400) {
					fprintf(stderr, "WRITEI stalled\n");
					return 1;
				}
				continue;
			}
			stall = 0;
			off += xf.result;
			done += xf.result;
		}
	}
	/* WRITEI returns once data is queued; let the last ~200 ms of
	 * buffer (4 x 50 ms periods) drain before close drops it. */
	usleep(300000);
	printf("played %ld frames (%.1f ms) from %s on %s\n",
	       done, done * 1000.0 / rate, path, dev);
	return 0;
}
