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
		 SNDRV_PCM_ACCESS_RW_INTERLEAVED);
	mask_one(&MASK(p, SNDRV_PCM_HW_PARAM_FORMAT),
		 SNDRV_PCM_FORMAT_S16_LE);
	mask_one(&MASK(p, SNDRV_PCM_HW_PARAM_SUBFORMAT),
		 SNDRV_PCM_SUBFORMAT_STD);
	ivl_set(&IVL(p, SNDRV_PCM_HW_PARAM_CHANNELS), 2);
	ivl_set(&IVL(p, SNDRV_PCM_HW_PARAM_RATE), rate);
	/* hw_params_choose() fixes unfixed params by min/max, so leave
	 * nothing it could pick inconsistently.  Pin values the rules
	 * engine derives EXACTLY: period_time 50 ms -> period_size
	 * 2400 at 48 kHz (9600 B -> 3 lldescs of 4092), periods 4 ->
	 * buffer 9600 frames/38400 B, all within .pcm_hardware and
	 * ESP32P4_MAX_DESCS.  An in-between frame count (e.g. 2048)
	 * can never be expressed as integer usec at 48 kHz and makes
	 * every mulkdiv-derived interval empty. */
	ivl_set(&IVL(p, SNDRV_PCM_HW_PARAM_PERIOD_TIME), 50000);
	ivl_set(&IVL(p, SNDRV_PCM_HW_PARAM_PERIODS), 4);
	p->rmask = ~0U;
	p->cmask = 0;

	err = ioctl(fd, SNDRV_PCM_IOCTL_HW_PARAMS, p);
	if (err < 0) {
		static const char *in[] = {
			"SAMPLE_BITS","FRAME_BITS","CHANNELS","RATE",
			"PERIOD_TIME","PERIOD_SIZE","PERIOD_BYTES","PERIODS",
			"BUFFER_TIME","BUFFER_SIZE","BUFFER_BYTES","TICK_TIME"};
		int i;
		perror("HW_PARAMS");
		err = ioctl(fd, SNDRV_PCM_IOCTL_HW_REFINE, p);
		fprintf(stderr, "re-refine err=%d\n", err);
		for (i = SNDRV_PCM_HW_PARAM_FIRST_INTERVAL;
		     i <= SNDRV_PCM_HW_PARAM_LAST_INTERVAL; i++) {
			struct snd_interval *v = &IVL(p, i);
			fprintf(stderr, "  %-12s [%u,%u] int=%d empty=%d\n",
				in[i - SNDRV_PCM_HW_PARAM_FIRST_INTERVAL],
				v->min, v->max, v->integer, v->empty);
		}
		for (i = 0; i < 3; i++)
			fprintf(stderr, "  mask%d bits0=%08x\n", i,
				p->masks[i].bits[0]);
		return 1;
	}
	err = ioctl(fd, SNDRV_PCM_IOCTL_PREPARE, 0);
	if (err < 0) {
		perror("PREPARE");
		return 1;
	}

	frames = (long)(rate * seconds);
	/* Exact one-period table: for integer f, the tone repeats every
	 * rate/gcd(f, rate) samples (440 -> 1200, 1000 -> 48), so one
	 * short sin() pass yields a sample-exact loop.  The recursive
	 * quadrature oscillator replaced in B15 had no such guarantee:
	 * its amplitude is 1/sin(w) (~17x at 440 Hz) -> int16 wrap, and
	 * a partial WRITEI desynced the state from the play position.
	 * pos tracks consumed frames so partial writes stay seamless. */
	unsigned fi = (unsigned)lrint(freq);
	unsigned g, period, i;
	long pos = 0;
	int16_t *tab;
	if (!fi)
		fi = 1;
	g = fi;
	{
		unsigned h = rate, t;
		while (h) {
			t = g % h;
			g = h;
			h = t;
		}
	}
	period = rate / g;
	if (period > rate)
		period = rate;
	tab = malloc(period * sizeof(int16_t));
	buf = malloc(960 * 2 * sizeof(int16_t));
	if (!tab || !buf) {
		fprintf(stderr, "malloc failed\n");
		return 1;
	}
	for (i = 0; i < period; i++) {
		long v = lrint(12000.0 * sin(2.0 * M_PI * fi * i / rate));
		tab[i] = v > 32767 ? 32767 : v < -32768 ? -32768 : v;
	}

	while (done < frames) {
		static int stall;
		long want = frames - done;
		if (want > 960)
			want = 960;
		for (long j = 0; j < want; j++) {
			int16_t v = tab[(pos + j) % period];
			buf[2 * j] = v;
			buf[2 * j + 1] = v;
		}
		xf.buf = buf;
		xf.frames = want;
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
		pos += xf.result;
		done += xf.result;
	}
	free(tab);
	printf("played %.0f ms of %.0f Hz sine on %s\n",
	       seconds * 1000, freq, dev);
	return 0;
}
