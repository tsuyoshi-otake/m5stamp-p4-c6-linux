// SPDX-License-Identifier: GPL-2.0-only
/*
 * Host-side test harness for wifi-qr -- NOT installed on the target.
 * Renders the same payload encoder as wifi-qr to a binary PGM so an
 * independent decoder (OpenCV QRCodeDetector, zxing-cpp) can verify it.
 *
 * usage: wqr_pbm CREDS_FILE OUT.pgm OUT.payload
 */
#include <stdio.h>
#include <string.h>

#include "wqr.h"
#include "qrcodegen.h"

#define SCALE 10

static uint8_t qr_tmp[WQR_BUF_LEN];
static uint8_t qr_buf[WQR_BUF_LEN];
static char payload[320];
static char ssid[64];
static char psk[96];
static uint8_t row[(45 + 2 * WQR_QUIET) * SCALE];

int main(int argc, char *argv[])
{
	FILE *f;
	int size, img, x, y;

	if (argc != 4) {
		fprintf(stderr, "usage: %s CREDS_FILE OUT.pgm OUT.payload\n",
			argv[0]);
		return 2;
	}
	if (wqr_read_creds(argv[1], ssid, sizeof(ssid), psk, sizeof(psk)) < 0) {
		fprintf(stderr, "wqr_pbm: no ssid=/psk= in %s\n", argv[1]);
		return 1;
	}
	if (wqr_payload(payload, sizeof(payload), ssid, psk) < 0) {
		fprintf(stderr, "wqr_pbm: credentials too long\n");
		return 1;
	}
	size = wqr_encode(payload, qr_tmp, qr_buf);
	if (size < 0) {
		fprintf(stderr, "wqr_pbm: payload too long for v%d\n",
			WQR_MAX_VERSION);
		return 1;
	}
	fprintf(stderr, "%s: %d modules, version %d, payload %d bytes\n",
		argv[2], size, (size - 17) / 4, (int)strlen(payload));

	img = (size + 2 * WQR_QUIET) * SCALE;
	f = fopen(argv[2], "wb");
	if (!f)
		return 1;
	fprintf(f, "P5\n%d %d\n255\n", img, img);
	for (y = 0; y < img; y++) {
		int my = y / SCALE - WQR_QUIET;

		for (x = 0; x < img; x++) {
			int mx = x / SCALE - WQR_QUIET;
			int dark = (mx >= 0 && my >= 0 && mx < size && my < size &&
				    qrcodegen_getModule(qr_buf, mx, my));
			row[x] = dark ? 0 : 255;
		}
		fwrite(row, 1, img, f);
	}
	fclose(f);

	f = fopen(argv[3], "wb");
	if (!f)
		return 1;
	fputs(payload, f);
	fputc('\n', f);
	fclose(f);
	return 0;
}
