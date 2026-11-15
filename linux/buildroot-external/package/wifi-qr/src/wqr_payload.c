// SPDX-License-Identifier: GPL-2.0-only
/*
 * Credentials parsing and WIFI: payload construction for wifi-qr.
 *
 * The payload follows the ZXing Wi-Fi Network config format:
 *   WIFI:T:WPA;S:<ssid>;P:<password>;;
 * Inside S: and P: the characters  \ ; , : "  must be escaped with a
 * backslash (ZXing barcode-contents documentation).
 */
#include <stdio.h>
#include <string.h>

#include "wqr.h"

static const char cred_keys[] = "\\;,:\"";
static char line[256];

static int needs_escape(char c)
{
	return c != '\0' && strchr(cred_keys, c) != NULL;
}

/* Append src to dst escaping cred_keys; keeps dst NUL-terminated.
 * Returns the number of bytes written, or -1 when out of room. */
static int esc_append(char *dst, size_t dst_len, size_t *pos, const char *src)
{
	while (*src) {
		size_t need = 1 + (needs_escape(*src) ? 1u : 0u);

		if (*pos + need + 1 > dst_len)
			return -1;
		if (needs_escape(*src))
			dst[(*pos)++] = '\\';
		dst[(*pos)++] = *src++;
	}
	dst[*pos] = '\0';
	return (int)*pos;
}

int wqr_read_creds(const char *path, char *ssid, size_t ssid_len,
		   char *psk, size_t psk_len)
{
	FILE *f;
	int got_ssid = 0, got_psk = 0;

	f = fopen(path, "r");
	if (!f)
		return -1;
	while (fgets(line, sizeof(line), f)) {
		char *v;
		size_t n;

		line[strcspn(line, "\r\n")] = '\0';
		if (!got_ssid && strncmp(line, "ssid=", 5) == 0) {
			v = line + 5;
			n = ssid_len;
			if (strlen(v) < n) {
				strcpy(ssid, v);
				got_ssid = 1;
			}
		} else if (!got_psk && strncmp(line, "psk=", 4) == 0) {
			v = line + 4;
			n = psk_len;
			if (strlen(v) < n) {
				strcpy(psk, v);
				got_psk = 1;
			}
		}
	}
	fclose(f);
	return (got_ssid && got_psk) ? 0 : -1;
}

int wqr_payload(char *out, size_t out_len, const char *ssid, const char *psk)
{
	static const char head[] = "WIFI:T:WPA;S:";
	static const char mid[] = ";P:";
	static const char tail[] = ";;";
	size_t pos = 0;

	if (!out_len)
		return -1;
	if (pos + sizeof(head) > out_len)
		return -1;
	memcpy(out + pos, head, sizeof(head) - 1);
	pos += sizeof(head) - 1;
	if (esc_append(out, out_len, &pos, ssid) < 0)
		return -1;
	if (pos + sizeof(mid) > out_len)
		return -1;
	memcpy(out + pos, mid, sizeof(mid) - 1);
	pos += sizeof(mid) - 1;
	if (esc_append(out, out_len, &pos, psk) < 0)
		return -1;
	if (pos + sizeof(tail) > out_len)
		return -1;
	memcpy(out + pos, tail, sizeof(tail) - 1);
	pos += sizeof(tail) - 1;
	out[pos] = '\0';
	return (int)pos;
}

int wqr_encode(const char *payload, uint8_t tmp[], uint8_t qr[])
{
	size_t len = strlen(payload);

	if (len > WQR_BUF_LEN)
		return -1;
	memcpy(tmp, payload, len);
	if (qrcodegen_encodeBinary(tmp, len, qr, qrcodegen_Ecc_MEDIUM,
				   1, WQR_MAX_VERSION, qrcodegen_Mask_AUTO, true))
		return qrcodegen_getSize(qr);
	memcpy(tmp, payload, len);
	if (qrcodegen_encodeBinary(tmp, len, qr, qrcodegen_Ecc_LOW,
				   1, WQR_MAX_VERSION, qrcodegen_Mask_AUTO, true))
		return qrcodegen_getSize(qr);
	return -1;
}
