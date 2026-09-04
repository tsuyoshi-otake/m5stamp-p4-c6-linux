// SPDX-License-Identifier: GPL-2.0-only

#define _GNU_SOURCE

#include <errno.h>
#include <fcntl.h>
#include <sched.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/syscall.h>
#include <unistd.h>

static int read_text(const char *path, char *buffer, size_t size)
{
	int fd;
	ssize_t got;

	if (size < 2)
		return -1;
	fd = open(path, O_RDONLY);
	if (fd < 0)
		return -1;
	got = read(fd, buffer, size - 1);
	close(fd);
	if (got < 0)
		return -1;
	buffer[got] = '\0';
	return (int)got;
}

static int current_cpu(void)
{
	unsigned int cpu = ~0U;

	if (syscall(SYS_getcpu, &cpu, NULL, NULL) < 0)
		return -1;
	return (int)cpu;
}

static int pin_cpu(unsigned int cpu)
{
	cpu_set_t set;

	CPU_ZERO(&set);
	CPU_SET(cpu, &set);
	return sched_setaffinity(0, sizeof(set), &set);
}

static int cpuinfo_processors(void)
{
	char line[128];
	FILE *stream;
	int count = 0;

	stream = fopen("/proc/cpuinfo", "r");
	if (!stream)
		return -1;
	while (fgets(line, sizeof(line), stream)) {
		if (!strncmp(line, "processor", 9))
			count++;
	}
	fclose(stream);
	return count;
}

static int run_cpu1_work(unsigned long *iterations)
{
	volatile uint32_t state = 0x13579bdf;
	unsigned long i;

	if (pin_cpu(1) < 0)
		return -1;
	for (i = 0; i < 4000000UL; i++) {
		state ^= state << 13;
		state ^= state >> 17;
		state ^= state << 5;
		if (!(i & 0x3ffffUL) && current_cpu() != 1)
			return -1;
	}
	*iterations = i;
	(void)state;
	return 0;
}

static int run_crosscall(char *result, size_t result_size)
{
	int fd;
	ssize_t written;

	if (pin_cpu(0) < 0)
		return -1;
	fd = open("/proc/m5stamp_smp_smoke", O_WRONLY);
	if (fd < 0)
		return -1;
	written = write(fd, "run\n", 4);
	close(fd);
	if (written != 4)
		return -1;
	if (read_text("/proc/m5stamp_smp_smoke", result, result_size) < 0)
		return -1;
	return strstr(result, "status=PASS") ? 0 : -1;
}

int main(void)
{
	char online[64];
	char crosscall[256];
	int processors;
	int user_cpu;
	unsigned long iterations;

	crosscall[0] = '\0';
	if (read_text("/sys/devices/system/cpu/online", online,
		      sizeof(online)) < 0) {
		perror("M5STAMP_L3SMOKE online");
		return 1;
	}
	online[strcspn(online, "\r\n")] = '\0';
	processors = cpuinfo_processors();
	printf("M5STAMP_L3SMOKE topology online=%s processors=%d\n",
	       online, processors);
	if (strcmp(online, "0-1") || processors != 2) {
		printf("M5STAMP_L3SMOKE FAIL topology\n");
		return 1;
	}

	if (run_cpu1_work(&iterations) < 0) {
		printf("M5STAMP_L3SMOKE FAIL cpu1-affine\n");
		return 1;
	}
	user_cpu = current_cpu();
	printf("M5STAMP_L3SMOKE cpu1-affine cpu=%d iterations=%lu\n",
	       user_cpu, iterations);
	if (user_cpu != 1) {
		printf("M5STAMP_L3SMOKE FAIL cpu1-affine cpu=%d\n", user_cpu);
		return 1;
	}

	if (run_crosscall(crosscall, sizeof(crosscall)) < 0) {
		printf("M5STAMP_L3SMOKE FAIL crosscall result=%s", crosscall);
		return 1;
	}
	printf("M5STAMP_L3SMOKE crosscall %s", crosscall);
	printf("M5STAMP_L3SMOKE PASS\n");
	return 0;
}
