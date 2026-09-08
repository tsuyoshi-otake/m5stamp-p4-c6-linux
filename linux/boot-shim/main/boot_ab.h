// SPDX-License-Identifier: Apache-2.0 OR GPL-2.0-only
#pragma once

#include <stdint.h>

/* Linux writes this request to BOOT_UPDATE_REQUEST_PA after it has copied and
 * read-back verified BOOT_IMAGE_BYTES at BOOT_LOAD_PA.  commit is written
 * last, so a reset while Linux is preparing the request is ignored. */
#define BOOT_IMAGE_BYTES          0x40000u
#define BOOT_UPDATE_REQUEST_PA    0x499bf000u
#define BOOT_REQUEST_MAGIC        0x45534243u /* "ESBC" */
#define BOOT_REQUEST_VERSION      1u
#define BOOT_REQUEST_COMMIT       0x434f4d4du /* "COMM" */

/* A committed metadata sector identifies a fully read-back-verified image.
 * The record checksum deliberately excludes commit_word; bootmeta sectors are
 * erased and programmed with commit_word last to make the record atomic. */
#define BOOT_META_MAGIC           0x4553424du /* "ESBM" */
#define BOOT_META_VERSION         1u
#define BOOT_META_COMMIT          0x434d4954u /* "CMIT" */

struct boot_update_request {
	uint32_t magic;
	uint32_t version;
	uint32_t image_bytes;
	uint32_t image_crc32;
	uint32_t commit_word;
} __attribute__((packed));

struct boot_metadata {
	uint32_t magic;
	uint32_t version;
	uint32_t generation;
	uint32_t slot;
	uint32_t image_bytes;
	uint32_t image_crc32;
	uint32_t record_crc32;
	uint32_t commit_word;
} __attribute__((packed));
