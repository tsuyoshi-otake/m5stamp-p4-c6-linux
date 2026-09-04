// SPDX-License-Identifier: GPL-2.0-only

#define _GNU_SOURCE

#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <sched.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/syscall.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#define DEFAULT_SECONDS 120U
#define MAX_SECONDS 600U
#define MEMORY_BYTES 16384U
#define TOP_PERIOD_MS 30000LL
#define TOP_CAPTURE_BYTES 4096U
#define TEMP_PERIOD_MS 1000LL

struct temperature_stats {
	unsigned long reads;
	unsigned long failures;
	int start_mc;
	int min_mc;
	int max_mc;
	int end_mc;
};

static const char *self_path;
static pid_t persistent_worker_pid = -1;

static void stop_persistent_worker(void)
{
	int status;

	if (persistent_worker_pid <= 0)
		return;
	(void)kill(persistent_worker_pid, SIGTERM);
	if (waitpid(persistent_worker_pid, &status, 0) == persistent_worker_pid)
		printf("M5STAMP_STRESS WORKER_STOP pid=%ld status=%d\n",
		       (long)persistent_worker_pid, status);
	else
		printf("M5STAMP_STRESS WORKER_STOP pid=%ld wait-fail errno=%d\n",
		       (long)persistent_worker_pid, errno);
	persistent_worker_pid = -1;
}

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

static int read_tsens_temperature(int *temperature_mc)
{
	char path[96];
	char type[64];
	char value_text[64];
	char *end;
	long value;
	unsigned int zone;

	for (zone = 0; zone < 8; zone++) {
		snprintf(path, sizeof(path), "/sys/class/thermal/thermal_zone%u/type",
			 zone);
		if (read_text(path, type, sizeof(type)) < 0)
			continue;
		type[strcspn(type, "\r\n")] = '\0';
		if (strcmp(type, "esp32p4-tsens"))
			continue;

		snprintf(path, sizeof(path), "/sys/class/thermal/thermal_zone%u/temp",
			 zone);
		if (read_text(path, value_text, sizeof(value_text)) < 0)
			return -1;
		errno = 0;
		end = NULL;
		value = strtol(value_text, &end, 10);
		while (end && (*end == ' ' || *end == '\t' ||
			       *end == '\r' || *end == '\n'))
			end++;
		if (errno || end == value_text || (end && *end) ||
		    value < -40000L || value > 125000L ||
		    value < INT_MIN || value > INT_MAX)
			return -1;
		*temperature_mc = (int)value;
		return 0;
	}
	return -1;
}

static void sample_temperature(struct temperature_stats *stats)
{
	int temperature_mc;

	if (read_tsens_temperature(&temperature_mc) < 0) {
		stats->failures++;
		return;
	}
	if (!stats->reads)
		stats->start_mc = temperature_mc;
	if (!stats->reads || temperature_mc < stats->min_mc)
		stats->min_mc = temperature_mc;
	if (!stats->reads || temperature_mc > stats->max_mc)
		stats->max_mc = temperature_mc;
	stats->end_mc = temperature_mc;
	stats->reads++;
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

static int64_t monotonic_ms(void)
{
	struct timespec now;

	if (clock_gettime(CLOCK_MONOTONIC, &now) < 0)
		return -1;
	return (int64_t)now.tv_sec * 1000LL + now.tv_nsec / 1000000LL;
}

static void short_sleep(unsigned long milliseconds)
{
	struct timespec request;

	request.tv_sec = milliseconds / 1000UL;
	request.tv_nsec = (long)(milliseconds % 1000UL) * 1000000L;
	while (nanosleep(&request, &request) < 0 && errno == EINTR)
		;
}

static int write_full(int fd, const char *buffer, size_t size)
{
	while (size) {
		ssize_t written = write(fd, buffer, size);

		if (written < 0 && errno == EINTR)
			continue;
		if (written <= 0)
			return -1;
		buffer += written;
		size -= (size_t)written;
	}
	return 0;
}

static int run_top_snapshot(const char *label)
{
	int output_pipe[2];
	pid_t child;
	char buffer[256];
	size_t retained = 0;
	int truncated = 0;
	ssize_t got;
	int status;

	if (pipe(output_pipe) < 0) {
		printf("M5STAMP_TOP FAIL label=%s pipe errno=%d\n", label, errno);
		return -1;
	}
	printf("M5STAMP_TOP BEGIN label=%s\n", label);
	fflush(stdout);
	/* NOMMU/uClibc exposes vfork() for this exec-only child path. */
	child = vfork();
	if (child < 0) {
		close(output_pipe[0]);
		close(output_pipe[1]);
		printf("M5STAMP_TOP FAIL label=%s errno=%d\n", label, errno);
		return -1;
	}
	if (child == 0) {
		close(output_pipe[0]);
		if (dup2(output_pipe[1], STDOUT_FILENO) < 0 ||
		    dup2(output_pipe[1], STDERR_FILENO) < 0)
			_exit(126);
		close(output_pipe[1]);
		execl("/bin/top", "top", "-b", "-n", "1", (char *)NULL);
		execl("/usr/bin/top", "top", "-b", "-n", "1", (char *)NULL);
		(void)write(STDOUT_FILENO, "M5STAMP_TOP exec-fail\n",
			    sizeof("M5STAMP_TOP exec-fail\n") - 1);
		_exit(127);
	}
	close(output_pipe[1]);
	while ((got = read(output_pipe[0], buffer, sizeof(buffer))) > 0) {
		size_t keep = 0;

		if (retained < TOP_CAPTURE_BYTES) {
			keep = (size_t)got;
			if (keep > TOP_CAPTURE_BYTES - retained)
				keep = TOP_CAPTURE_BYTES - retained;
			if (write_full(STDOUT_FILENO, buffer, keep) < 0) {
				close(output_pipe[0]);
				(void)waitpid(child, &status, 0);
				printf("M5STAMP_TOP FAIL label=%s write errno=%d\n",
				       label, errno);
				return -1;
			}
			retained += keep;
		}
		if ((size_t)got > keep)
			truncated = 1;
	}
	close(output_pipe[0]);
	if (got < 0) {
		(void)waitpid(child, &status, 0);
		printf("M5STAMP_TOP FAIL label=%s read errno=%d\n", label, errno);
		return -1;
	}
	if (waitpid(child, &status, 0) != child ||
	    !WIFEXITED(status) || WEXITSTATUS(status) != 0) {
		printf("M5STAMP_TOP FAIL label=%s status=%d\n", label, status);
		return -1;
	}
	if (truncated)
		printf("M5STAMP_TOP TRUNCATED label=%s limit=%u\n",
		       label, TOP_CAPTURE_BYTES);
	printf("M5STAMP_TOP END label=%s\n", label);
	return 0;
}

static int run_cpu1_worker(unsigned int seconds)
{
	volatile uint32_t state = 0x2468ace1U;
	unsigned long loops = 0;
	int cpu;
	int64_t deadline = -1;

	if (pin_cpu(1) < 0)
		return 10;
	if (seconds) {
		deadline = monotonic_ms();
		if (deadline < 0)
			return 12;
		deadline += (int64_t)seconds * 1000LL;
		dprintf(STDOUT_FILENO,
			"M5STAMP_STRESS WORKER_START cpu=%d seconds=%u\n",
			current_cpu(), seconds);
	}
	while (seconds ? monotonic_ms() < deadline : loops < 250000UL) {
		state ^= state << 13;
		state ^= state >> 17;
		state ^= state << 5;
		loops++;
		/*
		 * Keep a CPU1-affine worker from monopolising a non-preemptible
		 * NOMMU kernel.  The timer/IPI paths still get exercised, but the
		 * scheduler and workqueue receive a bounded opportunity to run.
		 */
		if (!(loops & 4095UL))
			short_sleep(1);
	}
	cpu = current_cpu();
	dprintf(STDOUT_FILENO, "M5STAMP_STRESS WORKER_END cpu=%d loops=%lu\n",
		cpu, loops);
	(void)state;
	return cpu == 1 ? 0 : 11;
}

static int run_cpu1_exec(void)
{
	pid_t child;
	int status;

	child = vfork();
	if (child < 0)
		return -1;
	if (child == 0) {
		execl(self_path, self_path, "--worker", (char *)NULL);
		(void)write(STDOUT_FILENO, "M5STAMP_STRESS exec-fail\n",
			    sizeof("M5STAMP_STRESS exec-fail\n") - 1);
		_exit(127);
	}
	if (waitpid(child, &status, 0) != child ||
	    !WIFEXITED(status) || WEXITSTATUS(status) != 0)
		return -1;
	return 0;
}

static int start_persistent_worker(unsigned int seconds)
{
	char seconds_arg[16];
	pid_t child;

	snprintf(seconds_arg, sizeof(seconds_arg), "%u", seconds);
	child = vfork();
	if (child < 0)
		return -1;
	if (child == 0) {
		execl(self_path, self_path, "--worker", "--seconds",
		      seconds_arg, (char *)NULL);
		(void)write(STDOUT_FILENO,
			    "M5STAMP_STRESS persistent-exec-fail\n",
			    sizeof("M5STAMP_STRESS persistent-exec-fail\n") - 1);
		_exit(127);
	}
	persistent_worker_pid = child;
	printf("M5STAMP_STRESS WORKER_SPAWN pid=%ld\n", (long)child);
	return 0;
}

static int persistent_worker_is_alive(void)
{
	int status;
	pid_t result;

	if (persistent_worker_pid <= 0)
		return -1;
	result = waitpid(persistent_worker_pid, &status, WNOHANG);
	if (result == 0)
		return 0;
	if (result == persistent_worker_pid) {
		printf("M5STAMP_STRESS WORKER_EXIT status=%d\n", status);
		persistent_worker_pid = -1;
		return -1;
	}
	return -1;
}

static int run_memory_probe(unsigned long iteration)
{
	unsigned char *buffer;
	unsigned long i;
	uint32_t checksum = 0;

	buffer = malloc(MEMORY_BYTES);
	if (!buffer)
		return -1;
	for (i = 0; i < MEMORY_BYTES; i++) {
		buffer[i] = (unsigned char)(iteration + i * 13U);
		checksum += buffer[i];
	}
	free(buffer);
	return checksum ? 0 : -1;
}

static int run_crosscall(void)
{
	char result[256];
	int fd;
	ssize_t written;
	ssize_t got;

	if (pin_cpu(0) < 0)
		return -1;
	fd = open("/proc/m5stamp_smp_smoke", O_WRONLY);
	if (fd < 0)
		return -1;
	written = write(fd, "run\n", 4);
	close(fd);
	if (written != 4)
		return -1;
	fd = open("/proc/m5stamp_smp_smoke", O_RDONLY);
	if (fd < 0)
		return -1;
	got = read(fd, result, sizeof(result) - 1);
	close(fd);
	if (got < 0)
		return -1;
	result[got] = '\0';
	return strstr(result, "status=PASS") ? 0 : -1;
}

static int parse_seconds(int argc, char **argv, unsigned int *seconds)
{
	char *end;
	unsigned long value;
	int i;

	*seconds = DEFAULT_SECONDS;
	for (i = 1; i < argc; i++) {
		if (!strcmp(argv[i], "--worker"))
			continue;
		if (!strcmp(argv[i], "--seconds") && i + 1 < argc) {
			errno = 0;
			value = strtoul(argv[++i], &end, 10);
			if (errno || *end || value == 0 || value > MAX_SECONDS)
				return -1;
			*seconds = (unsigned int)value;
			continue;
		}
		return -1;
	}
	return 0;
}

int main(int argc, char **argv)
{
	char online[64];
	unsigned int seconds;
	unsigned long iterations = 0;
	unsigned long top_snapshots = 0;
	unsigned long worker_execs = 0;
	unsigned long smoke_calls = 0;
	int processors;
	int64_t started;
	int64_t deadline;
	int64_t next_top;
	int64_t next_temperature;
	struct temperature_stats temperature = { 0 };

	setvbuf(stdout, NULL, _IOLBF, 0);
	self_path = argv[0];
	if (!strchr(self_path, '/'))
		self_path = "/usr/sbin/m5stamp-smp-stress";
	if (argc > 1 && !strcmp(argv[1], "--worker")) {
		unsigned int worker_seconds = 0;

		if (argc > 2) {
			if (parse_seconds(argc - 1, argv + 1, &worker_seconds) < 0)
				return 2;
		}
		return run_cpu1_worker(worker_seconds);
	}
	if (parse_seconds(argc, argv, &seconds) < 0) {
		printf("M5STAMP_STRESS FAIL usage\n");
		return 2;
	}
	atexit(stop_persistent_worker);
	if (pin_cpu(0) < 0) {
		printf("M5STAMP_STRESS FAIL pin-cpu0 errno=%d\n", errno);
		return 1;
	}
	if (read_text("/sys/devices/system/cpu/online", online,
		      sizeof(online)) < 0) {
		printf("M5STAMP_STRESS FAIL online\n");
		return 1;
	}
	online[strcspn(online, "\r\n")] = '\0';
	processors = cpuinfo_processors();
	printf("M5STAMP_STRESS topology online=%s processors=%d seconds=%u\n",
	       online, processors, seconds);
	if (strcmp(online, "0-1") || processors != 2) {
		printf("M5STAMP_STRESS FAIL topology\n");
		return 1;
	}
	if (run_top_snapshot("start") < 0)
		return 1;
	top_snapshots++;
	if (start_persistent_worker(seconds) < 0) {
		printf("M5STAMP_STRESS FAIL persistent-worker\n");
		return 1;
	}

	started = monotonic_ms();
	if (started < 0)
		return 1;
	deadline = started + (int64_t)seconds * 1000LL;
	next_top = started + TOP_PERIOD_MS;
	next_temperature = started;
	sample_temperature(&temperature);
	next_temperature += TEMP_PERIOD_MS;
	while (monotonic_ms() < deadline) {
		int64_t now;

		if (persistent_worker_is_alive() < 0) {
			printf("M5STAMP_STRESS FAIL persistent-worker-exit\n");
			return 1;
		}
		if (run_memory_probe(iterations) < 0) {
			printf("M5STAMP_STRESS FAIL memory iteration=%lu\n",
			       iterations);
			return 1;
		}
		if (run_cpu1_exec() < 0) {
			printf("M5STAMP_STRESS FAIL cpu1-exec iteration=%lu\n",
			       iterations);
			return 1;
		}
		worker_execs++;
		if (run_crosscall() < 0) {
			printf("M5STAMP_STRESS FAIL crosscall iteration=%lu\n",
			       iterations);
			return 1;
		}
		smoke_calls++;
		iterations++;
		if (!(iterations & 15UL))
			printf("M5STAMP_STRESS progress iteration=%lu cpu=%d\n",
			       iterations, current_cpu());
		if (monotonic_ms() >= next_top) {
			char label[32];

			snprintf(label, sizeof(label), "iteration-%lu", iterations);
			if (run_top_snapshot(label) < 0)
				return 1;
			top_snapshots++;
			next_top += TOP_PERIOD_MS;
		}
		now = monotonic_ms();
		if (now >= next_temperature) {
			sample_temperature(&temperature);
			next_temperature += TEMP_PERIOD_MS;
		}
		short_sleep(10);
	}
	if (run_top_snapshot("end") < 0)
		return 1;
	top_snapshots++;
	sample_temperature(&temperature);
	stop_persistent_worker();
	printf("M5STAMP_STRESS PASS seconds=%u iterations=%lu "
	       "top_snapshots=%lu worker_execs=%lu smoke_calls=%lu "
	       "temp_reads=%lu temp_failures=%lu temp_start_mc=%d "
	       "temp_min_mc=%d temp_max_mc=%d temp_end_mc=%d\n",
	       seconds, iterations, top_snapshots, worker_execs, smoke_calls,
	       temperature.reads, temperature.failures, temperature.start_mc,
	       temperature.min_mc, temperature.max_mc, temperature.end_mc);
	return 0;
}
