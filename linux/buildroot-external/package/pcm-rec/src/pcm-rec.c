/* Minimal NOMMU recorder: capture a few seconds from a capture PCM
 * using only the SNDRV_PCM ioctl ABI, write s16le to a file, and run
 * a Goertzel at a target frequency over 0.5 s windows so the #24/B12
 * mic loopback can be verified on the board without a PC-side FFT.
 *
 * Usage: pcm-rec [card [seconds [analyze_hz [file]]]]
 *   pcm-rec            -> 1 s from pcmC0D0c, no analysis
 *   pcm-rec 1 3 1000   -> 3 s from pcmC1D0c, 1 kHz bins to stderr
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

int main(int argc, char **argv)
{
	int card = argc > 1 ? atoi(argv[1]) : 0;
	double seconds = argc > 2 ? atof(argv[2]) : 1.0;
	double target = argc > 3 ? atof(argv[3]) : 0.0;
	const char *out = argc > 4 ? argv[4] : "/tmp/rec.pcm";
	unsigned int rate = 48000;
	char dev[64];
	struct snd_pcm_hw_params *p;
	struct snd_xferi xf;
	int16_t *buf;
	long frames, done = 0;
	int fd, ofd = -1, err, m;

	snprintf(dev, sizeof(dev), "/dev/snd/pcmC%dD0c", card);
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

	/* Same pin set as pcm-sine: RW_INTERLEAVED S16_LE stereo
	 * 48 kHz, 50 ms period x4 -- inside .pcm_hardware. */
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
	/* A capture stream does not run until START; WRITEI auto-
	 * starts playback but there is no equivalent implicit
	 * trigger for reads. */
	err = ioctl(fd, SNDRV_PCM_IOCTL_START, 0);
	if (err < 0) {
		perror("START");
		return 1;
	}

	frames = (long)(rate * seconds);
	buf = malloc(frames * 2 * sizeof(int16_t));
	if (!buf) {
		fprintf(stderr, "malloc %ld failed\n", frames);
		return 1;
	}
	if (out && out[0] != '-') {
		ofd = open(out, O_WRONLY | O_CREAT | O_TRUNC, 0644);
		if (ofd < 0)
			perror(out);
	}

	while (done < frames) {
		static int stall;
		xf.buf = buf + done * 2;
		xf.frames = frames - done;
		xf.result = 0;
		err = ioctl(fd, SNDRV_PCM_IOCTL_READI_FRAMES, &xf);
		if (err < 0) {
			if (errno == EAGAIN || errno == EINTR)
				continue;
			perror("READI_FRAMES");
			return 1;
		}
		if (xf.result <= 0) {
			if (++stall > 400) {
				fprintf(stderr, "READI stalled\n");
				return 1;
			}
			continue;
		}
		stall = 0;
		done += xf.result;
	}
	if (ofd >= 0) {
		if (write(ofd, buf, done * 2 * sizeof(int16_t)) < 0)
			perror("write");
		close(ofd);
	}

	printf("recorded %.0f ms from %s -> %s\n",
	       seconds * 1000, dev, ofd >= 0 ? out : "(no file)");

	if (target > 0.0) {
		/* Goertzel per half-second window, channel 0.  Report
		 * the target bin magnitude in dBFS next to the window
		 * RMS so a silent capture is unambiguous. */
		long win = rate / 2;
		double w0 = 2.0 * M_PI * target / rate;
		double cw = 2.0 * cos(w0);
		for (long b = 0; b + win <= done; b += win) {
			double s0 = 0, s1 = 0, s2 = 0, mag, rms = 0;
			for (long i = 0; i < win; i++) {
				double x = buf[(b + i) * 2];
				s0 = x + cw * s1 - s2;
				s2 = s1;
				s1 = s0;
				rms += x * x;
			}
			mag = sqrt(s1 * s1 + s2 * s2 - cw * s1 * s2) / (win / 2);
			rms = sqrt(rms / win) / 32768.0;
			fprintf(stderr, "t=%5.1fs %.0fHz=%6.1fdBFS rms=%.4f\n",
				(double)b / rate, target,
				20.0 * log10(mag / 32768.0 + 1e-9), rms);
		}
	}
	return 0;
}
