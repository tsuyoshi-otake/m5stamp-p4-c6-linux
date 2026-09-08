#define _GNU_SOURCE
/*
 * Stage a verified boot image in PSRAM for boot-shim's next-reboot A/B
 * commit.  This program never writes flash.  The request's commit word is
 * written last, so an interrupted staging operation is ignored by firmware.
 */
#include <errno.h>
#include <fcntl.h>
#include <inttypes.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>

#define EASYSTICK_PSRAM_REQUEST_PHYS UINT32_C(0x499bf000)
#define EASYSTICK_PSRAM_IMAGE_PHYS   UINT32_C(0x499c0000)
#define EASYSTICK_IMAGE_BYTES        UINT32_C(0x00040000)
#define EASYSTICK_REQUEST_MAGIC      UINT32_C(0x45534243) /* "ESBC" */
#define EASYSTICK_REQUEST_VERSION    UINT32_C(1)
#define EASYSTICK_REQUEST_COMMIT     UINT32_C(0x434f4d4d) /* "COMM" */

struct easystick_commit_request {
	uint32_t magic;
	uint32_t version;
	uint32_t image_bytes;
	uint32_t image_crc32;
	uint32_t commit;
} __attribute__((packed));

_Static_assert(sizeof(struct easystick_commit_request) == 20,
	"commit request layout changed");

static uint32_t crc32_ieee_update(uint32_t crc, const uint8_t *buf, size_t len)
{
	while (len-- != 0) {
		unsigned int bit;
		crc ^= *buf++;
		for (bit = 0; bit < 8; ++bit)
			crc = (crc >> 1) ^ ((crc & 1U) ? UINT32_C(0xedb88320) : 0U);
	}
	return crc;
}

static uint32_t crc32_ieee(const uint8_t *buf, size_t len)
{
	return ~crc32_ieee_update(UINT32_MAX, buf, len);
}

static int read_exact(int fd, uint8_t *dst, size_t len)
{
	while (len != 0) {
		ssize_t got = read(fd, dst, len);
		if (got < 0) {
			if (errno == EINTR)
				continue;
			return -1;
		}
		if (got == 0) {
			errno = EIO;
			return -1;
		}
		dst += (size_t)got;
		len -= (size_t)got;
	}
	return 0;
}

static void *map_physical(int memfd, uint32_t phys, size_t length)
{
	void *mapped = mmap(NULL, length, PROT_READ | PROT_WRITE, MAP_SHARED,
				    memfd, (off_t)phys);
	return mapped == MAP_FAILED ? NULL : mapped;
}

static int stage_image(int imagefd, volatile uint8_t *psram, uint32_t *crc_out)
{
	uint8_t buffer[4096];
	size_t remaining = EASYSTICK_IMAGE_BYTES;
	size_t offset = 0;
	uint32_t crc = UINT32_MAX;

	while (remaining != 0) {
		size_t chunk = remaining < sizeof(buffer) ? remaining : sizeof(buffer);
		if (read_exact(imagefd, buffer, chunk) != 0)
			return -1;
		crc = crc32_ieee_update(crc, buffer, chunk);
		for (size_t i = 0; i < chunk; ++i)
			psram[offset + i] = buffer[i];
		offset += chunk;
		remaining -= chunk;
	}
	*crc_out = ~crc;
	return 0;
}

static uint32_t crc32_volatile(const volatile uint8_t *buf, size_t len)
{
	uint32_t crc = UINT32_MAX;
	while (len-- != 0) {
		unsigned int bit;
		crc ^= *buf++;
		for (bit = 0; bit < 8; ++bit)
			crc = (crc >> 1) ^ ((crc & 1U) ? UINT32_C(0xedb88320) : 0U);
	}
	return ~crc;
}

static void publish_request(volatile struct easystick_commit_request *request,
				    uint32_t crc)
{
	/* Metadata must be visible before the final commit store. */
	request->magic = EASYSTICK_REQUEST_MAGIC;
	request->version = EASYSTICK_REQUEST_VERSION;
	request->image_bytes = EASYSTICK_IMAGE_BYTES;
	request->image_crc32 = crc;
	__sync_synchronize();
	request->commit = EASYSTICK_REQUEST_COMMIT;
	__sync_synchronize();
}

int main(int argc, char **argv)
{
	const char *mem_path = "/dev/mem";
	struct stat st;
	int imagefd = -1;
	int memfd = -1;
	void *image_map = NULL;
	void *request_map = NULL;
	uint32_t expected_crc;
	int rc = EXIT_FAILURE;

	if (argc == 2 && strcmp(argv[1], "--self-test") == 0) {
		static const uint8_t test_data[] = "123456789";
		if (crc32_ieee(test_data, sizeof(test_data) - 1) != UINT32_C(0xcbf43926)) {
			fprintf(stderr, "boot commit: CRC32 self-test failed\n");
			return EXIT_FAILURE;
		}
		return EXIT_SUCCESS;
	}
	if (argc != 2) {
		fprintf(stderr, "Usage: %s <validated-256KiB-boot.img>\n", argv[0]);
		return EXIT_FAILURE;
	}
	imagefd = open(argv[1], O_RDONLY | O_CLOEXEC);
	if (imagefd < 0) {
		fprintf(stderr, "boot commit: cannot open %s: %s\n", argv[1], strerror(errno));
		goto out;
	}
	if (fstat(imagefd, &st) != 0) {
		fprintf(stderr, "boot commit: cannot stat %s: %s\n", argv[1], strerror(errno));
		goto out;
	}
	if (!S_ISREG(st.st_mode) || st.st_size != (off_t)EASYSTICK_IMAGE_BYTES) {
		fprintf(stderr, "boot commit: input must be a regular %u-byte image\n",
			EASYSTICK_IMAGE_BYTES);
		goto out;
	}
	memfd = open(mem_path, O_RDWR | O_SYNC | O_CLOEXEC);
	if (memfd < 0) {
		fprintf(stderr, "boot commit: cannot open %s: %s\n", mem_path, strerror(errno));
		goto out;
	}
	image_map = map_physical(memfd, EASYSTICK_PSRAM_IMAGE_PHYS, EASYSTICK_IMAGE_BYTES);
	request_map = map_physical(memfd, EASYSTICK_PSRAM_REQUEST_PHYS,
				   sizeof(struct easystick_commit_request));
	if (image_map == NULL || request_map == NULL) {
		fprintf(stderr, "boot commit: cannot map PSRAM staging area: %s\n", strerror(errno));
		goto out;
	}
	/* Invalidate a prior request before changing any image byte. */
	((volatile struct easystick_commit_request *)request_map)->commit = 0;
	__sync_synchronize();
	if (((volatile struct easystick_commit_request *)request_map)->commit != 0) {
		fprintf(stderr, "boot commit: could not clear prior request\n");
		goto out;
	}
	if (stage_image(imagefd, image_map, &expected_crc) != 0 ||
	    crc32_volatile(image_map, EASYSTICK_IMAGE_BYTES) != expected_crc) {
		fprintf(stderr, "boot commit: PSRAM image readback CRC failed\n");
		goto out;
	}
	publish_request(request_map, expected_crc);
	if (((volatile struct easystick_commit_request *)request_map)->magic != EASYSTICK_REQUEST_MAGIC ||
	    ((volatile struct easystick_commit_request *)request_map)->version != EASYSTICK_REQUEST_VERSION ||
	    ((volatile struct easystick_commit_request *)request_map)->image_bytes != EASYSTICK_IMAGE_BYTES ||
	    ((volatile struct easystick_commit_request *)request_map)->image_crc32 != expected_crc ||
	    ((volatile struct easystick_commit_request *)request_map)->commit != EASYSTICK_REQUEST_COMMIT) {
		fprintf(stderr, "boot commit: request readback failed\n");
		goto out;
	}
	fprintf(stderr, "boot commit: staged %u bytes, crc32=%08" PRIx32 "\n",
		EASYSTICK_IMAGE_BYTES, expected_crc);
	rc = EXIT_SUCCESS;
out:
	if (request_map != NULL)
		munmap(request_map, sizeof(struct easystick_commit_request));
	if (image_map != NULL)
		munmap(image_map, EASYSTICK_IMAGE_BYTES);
	if (memfd >= 0)
		close(memfd);
	if (imagefd >= 0)
		close(imagefd);
	return rc;
}
