/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * Shared helpers for wifi-qr and its host test harness: read the
 * credentials file, build the ZXing WIFI: payload, encode the QR matrix.
 */
#ifndef WQR_H
#define WQR_H

#include <stddef.h>
#include <stdint.h>

#include "qrcodegen.h"

/* Largest QR version produced; keeps the encoder's buffers small. */
#define WQR_MAX_VERSION 7

/* Quiet zone in modules on every side of the code. */
#define WQR_QUIET 4

/* Buffer sizes for wqr_encode; both arrays must be this long. */
#define WQR_BUF_LEN qrcodegen_BUFFER_LEN_FOR_VERSION(WQR_MAX_VERSION)

/* Read ssid= and psk= from a credentials.txt-style file.  Each output
 * buffer gets the raw value (no escapes added).  Returns 0 when both
 * were found, -1 otherwise. */
int wqr_read_creds(const char *path, char *ssid, size_t ssid_len,
		   char *psk, size_t psk_len);

/* Build "WIFI:T:WPA;S:<ssid>;P:<psk>;;" with ZXing escaping of
 * \ ; , : " inside the S: and P: values.  Returns the payload length,
 * or -1 when it does not fit out_len. */
int wqr_payload(char *out, size_t out_len, const char *ssid, const char *psk);

/* Encode payload as a byte-mode QR.  tmp and qr must each be at least
 * WQR_BUF_LEN bytes.  Tries ECC level M, then L.  Returns the module
 * count per side (17+4*version), or -1 when the payload exceeds the
 * version/ECC limits. */
int wqr_encode(const char *payload, uint8_t tmp[], uint8_t qr[]);

#endif
