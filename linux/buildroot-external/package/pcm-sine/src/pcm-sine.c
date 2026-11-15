/* Minimal NOMMU aplay: a short sine on the first playback PCM.
 * alsa-utils needs fork() (BR2_USE_MMU) and tinyalsa needs dynamic
 * linking, so neither can build on this static NOMMU image.  This
 * speaks the raw SNDRV_PCM ioctl ABI only -- no libasound (#24).
 *
 * Usage: pcm-sine [card [seconds [freq_hz]]]
 *   pcm-sine            -> 1 s 440 Hz on pcmC0D0p
 *   pcm-sine 0 2 880    -> 2 s 880 Hz on pcmC0D0p
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
	double freq = argc > 3 ? atof(argv[3]) : 440.0;
	unsigned int rate = 48000;
	char dev[64];
	struct snd_pcm_hw_params *p;
	struct snd_xferi xf;
	int16_t *buf;
	long frames, done = 0;
	int fd, err, m;

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

	/* Pin: RW_INTERLEAVED, S16_LE, stereo, 48 kHz. */
	mask_one(&MASK(p, SNDRV_PCM_HW_PARAM_ACCESS),
		 SND_PCM_ACCESS_RW_INTERLEAVED);
	mask_one(&MASK(p, SNDRV_PCM_HW_PARAM_FORMAT),
		 SND_PCM_FORMAT_S16_LE);
	mask_one(&MASK(p, SNDRV_PCM_HW_PARAM_SUBFORMAT),
		 SND_PCM_SUBFORMAT_STD);
	ivl_set(&IVL(p, SNDRV_PCM_HW_PARAM_CHANNELS), 2);
	ivl_set(&IVL(p, SNDRV_PCM_HW_PARAM_RATE), rate);
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

	frames = (long)(rate * seconds);
	buf = malloc(frames * 2 * sizeof(int16_t));
	if (!buf) {
		fprintf(stderr, "malloc %ld failed\n", frames);
		return 1;
	}
	for (long i = 0; i < frames; i++) {
		int16_t s = (int16_t)(12000.0 * sin(2.0 * M_PI * freq * i / rate));
		buf[2 * i] = s;
		buf[2 * i + 1] = s;
	}

	while (done < frames) {
		xf.buf = buf + done * 2;
		xf.frames = frames - done;
		err = ioctl(fd, SNDRV_PCM_IOCTL_WRITEI_FRAMES, &xf);
		if (err <= 0) {
			if (err == -EAGAIN || err == -EINTR)
				continue;
			perror("WRITEI_FRAMES");
			return 1;
		}
		done += err;
	}
	printf("played %.0f ms of %.0f Hz sine on %s\n",
	       seconds * 1000, freq, dev);
	return 0;
}
