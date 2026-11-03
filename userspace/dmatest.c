// SPDX-License-Identifier: GPL-2.0
/*
 * End-to-end check of /dev/dmaeng. Exits non-zero on any failure.
 *
 *   dmatest                    the full suite
 *   dmatest --short            small workload, for a trace worth committing
 *   dmatest --faults           error paths; needs a FAULT_INJECT=1 driver build
 *   dmatest --bench FILE [N]   N timed transfers per path and size, raw CSV
 */
#define _GNU_SOURCE
#include <endian.h>
#include <errno.h>
#include <fcntl.h>
#include <sched.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <sys/time.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#include "dmaeng_regs.h"
#include "dmaeng_uapi.h"

#define SRC_OFF		0ULL
#define DST_OFF		(128ULL * 1024)
#define COPY_LEN	4096

#define NR_SUBMITTERS	4
#define SUBMITTER_COPIES 200
#define SUBMITTER_STRIDE (256ULL * 1024)

#define BENCH_WARMUP	1000
#define BENCH_DEFAULT_N	10000
#define POLL_TIMEOUT_NS	2000000000ULL

enum path { PATH_BLOCKING, PATH_KERNEL_POLL, PATH_USER_POLL, NR_PATHS };

static const uint32_t path_flags[NR_PATHS] = {
	[PATH_BLOCKING] = 0,
	[PATH_KERNEL_POLL] = DMAENG_XFER_POLL,
	[PATH_USER_POLL] = DMAENG_XFER_NOWAIT,
};

static const char * const path_names[NR_PATHS] = {
	[PATH_BLOCKING] = "blocking",
	[PATH_KERNEL_POLL] = "kernel_poll",
	[PATH_USER_POLL] = "user_poll",
};

static const uint8_t *ring;	/* read-only view of the descriptor ring */
static int failures;

#define CHECK(cond, ...) do {					\
	if (!(cond)) {						\
		printf("FAIL %s:%d: ", __func__, __LINE__);	\
		printf(__VA_ARGS__);				\
		printf("\n");					\
		failures++;					\
	}							\
} while (0)

static int submit(int fd, uint64_t src, uint64_t dst, uint32_t len, uint32_t flags)
{
	struct dmaeng_xfer x = {
		.src_off = src, .dst_off = dst, .len = len, .flags = flags,
	};

	return ioctl(fd, DMAENG_SUBMIT, &x) ? -errno : 0;
}

static uint64_t now_ns(void)
{
	struct timespec ts;

	clock_gettime(CLOCK_MONOTONIC, &ts);
	return ts.tv_sec * 1000000000ULL + ts.tv_nsec;
}

static uint32_t ring_field(uint32_t idx, uint32_t off)
{
	const uint32_t *p = (const void *)(ring + idx * DMAENG_DESC_SIZE + off);

	/* Acquire: status is only read after OWN is seen clear (spec 5.6 rule 5) */
	return le32toh(__atomic_load_n(p, __ATOMIC_ACQUIRE));
}

/*
 * Path 3: spin on OWN from userspace. Returns the descriptor status or
 * -ETIMEDOUT. The clock is read rarely so the spin stays a plain memory read.
 */
static int poll_ring(uint32_t idx)
{
	uint64_t start = now_ns();
	uint32_t spins = 0;

	while (ring_field(idx, DMAENG_DESC_OFF_FLAGS) & DMAENG_DESC_OWN) {
		if (!(++spins & 0xfffff) && now_ns() - start > POLL_TIMEOUT_NS)
			return -ETIMEDOUT;
	}
	return ring_field(idx, DMAENG_DESC_OFF_STATUS);
}

/* One verified copy. The guard byte catches a device that overruns len. */
static void copy_at(int fd, uint8_t *buf, uint64_t src_off, uint64_t dst_off,
		    uint32_t len, uint8_t seed, enum path path)
{
	uint8_t *src = buf + src_off;
	uint8_t *dst = buf + dst_off;
	struct dmaeng_xfer x = {
		.src_off = src_off, .dst_off = dst_off, .len = len,
		.flags = path_flags[path],
	};
	uint8_t want;
	uint32_t i;
	int ret;

	/* Position-dependent, so a stuck or shifted copy cannot pass */
	for (i = 0; i < len; i++)
		src[i] = seed + i * 31;
	memset(dst, ~seed, len + 1);
	want = ~seed;

	if (ioctl(fd, DMAENG_SUBMIT, &x)) {
		CHECK(0, "%s len %u: submit: %s", path_names[path], len,
		      strerror(errno));
		return;
	}
	if (path == PATH_USER_POLL) {
		ret = poll_ring(x.desc_idx);
		CHECK(ret == 0, "%s len %u: poll returned %d", path_names[path],
		      len, ret);
	} else {
		CHECK(x.lat_ns > 0, "%s len %u: lat_ns not filled",
		      path_names[path], len);
	}
	CHECK(!memcmp(src, dst, len), "%s len %u: data mismatch",
	      path_names[path], len);
	CHECK(dst[len] == want, "%s len %u: wrote past the end",
	      path_names[path], len);
}

static void copy_and_verify(int fd, uint8_t *buf, uint32_t len, uint8_t seed)
{
	copy_at(fd, buf, SRC_OFF, DST_OFF, len, seed, PATH_BLOCKING);
}

/* Each path has to wrap the ring too, since neither polled one interrupts */
static void test_paths(int fd, uint8_t *buf, const struct dmaeng_info *info)
{
	uint32_t i, n = info->ring_entries + 8;
	int p;

	for (p = 0; p < NR_PATHS; p++) {
		for (i = 0; i < n; i++)
			copy_at(fd, buf, SRC_OFF, DST_OFF,
				1 + (i * 4099) % info->max_xfer, i, p);
		printf("path %s: %u copies\n", path_names[p], n);
	}
	CHECK(submit(fd, SRC_OFF, DST_OFF, 1,
		     DMAENG_XFER_POLL | DMAENG_XFER_NOWAIT) == -EINVAL,
	      "two paths at once");
}

/*
 * A no-wait submit reports nothing itself. The device's verdict is in the
 * ring, and the next submit must restart the device it stopped.
 */
static void test_nowait_error(int fd, uint8_t *buf, const struct dmaeng_info *info)
{
	struct dmaeng_xfer x = {
		.src_off = SRC_OFF, .dst_off = DST_OFF,
		.len = info->max_xfer + 1, .flags = DMAENG_XFER_NOWAIT,
	};
	int ret;

	CHECK(!ioctl(fd, DMAENG_SUBMIT, &x), "submit: %s", strerror(errno));
	ret = poll_ring(x.desc_idx);
	CHECK(ret == DMAENG_ERR_BAD_LEN, "ring status %d, want %d", ret,
	      DMAENG_ERR_BAD_LEN);
	copy_at(fd, buf, SRC_OFF, DST_OFF, COPY_LEN, 0x44, PATH_USER_POLL);
	printf("no-wait error seen in the ring, driver recovered\n");
}

/* More copies than ring slots: only a correct wrap survives this */
static void test_ring_wrap(int fd, uint8_t *buf, const struct dmaeng_info *info)
{
	uint32_t i, n = info->ring_entries * 2 + 8;

	for (i = 0; i < n; i++)
		copy_and_verify(fd, buf, 1 + (i * 4099) % info->max_xfer, i);
	copy_and_verify(fd, buf, info->max_xfer, 0x5a);
	printf("ring wrap: %u copies, 1 B to %u B\n", n + 1, info->max_xfer);
}

/* The security boundary: no offset may reach outside the buffer */
static void test_rejected_by_driver(int fd, const struct dmaeng_info *info)
{
	uint64_t size = info->buf_bytes;

	CHECK(submit(fd, size, DST_OFF, 1, 0) == -EINVAL, "src past end");
	CHECK(submit(fd, SRC_OFF, size, 1, 0) == -EINVAL, "dst past end");
	CHECK(submit(fd, size - 1, DST_OFF, 2, 0) == -EINVAL, "src straddles end");
	CHECK(submit(fd, UINT64_MAX, DST_OFF, 2, 0) == -EINVAL, "src wraps u64");
	CHECK(submit(fd, SRC_OFF, UINT64_MAX, 2, 0) == -EINVAL, "dst wraps u64");
	CHECK(submit(fd, SRC_OFF, 16, 32, 0) == -EINVAL, "overlap");
	CHECK(submit(fd, SRC_OFF, DST_OFF, 1, 1U << 31) == -EINVAL, "flags set");
	printf("driver rejected bad offsets, overlap and flags\n");
}

/*
 * The device, not the driver, rejects these and then stops until reset.
 * A later copy working proves the driver recovered it.
 */
static void test_device_error_recovers(int fd, uint8_t *buf,
				       const struct dmaeng_info *info)
{
	uint8_t *dst = buf + DST_OFF;

	memset(dst, 0xee, 16);
	CHECK(submit(fd, SRC_OFF, DST_OFF, info->max_xfer + 1, 0) == -EINVAL,
	      "oversize not refused");
	CHECK(dst[0] == 0xee, "oversize transfer moved data");
	copy_and_verify(fd, buf, COPY_LEN, 0x11);

	CHECK(submit(fd, SRC_OFF, DST_OFF, 0, 0) == -EINVAL,
	      "zero length not refused");
	copy_and_verify(fd, buf, COPY_LEN, 0x22);
	printf("device errors reported, driver recovered\n");
}

/*
 * The mutex must keep the ring at one transfer in flight. Each process
 * copies in its own region, so any failure means transfers interfered.
 */
static void test_concurrent_submitters(int fd, uint8_t *buf)
{
	pid_t pids[NR_SUBMITTERS];
	int i, c, status;

	fflush(stdout);
	for (i = 0; i < NR_SUBMITTERS; i++) {
		uint64_t src = i * SUBMITTER_STRIDE;

		pids[i] = fork();
		CHECK(pids[i] >= 0, "fork: %s", strerror(errno));
		if (pids[i])
			continue;
		for (c = 0; c < SUBMITTER_COPIES; c++)
			copy_at(fd, buf, src, src + DST_OFF, COPY_LEN, c + i,
				(c + i) % PATH_USER_POLL);
		fflush(stdout);
		_exit(failures ? 1 : 0);
	}
	for (i = 0; i < NR_SUBMITTERS; i++) {
		if (pids[i] <= 0)
			continue;
		CHECK(waitpid(pids[i], &status, 0) == pids[i] &&
		      WIFEXITED(status) && !WEXITSTATUS(status),
		      "submitter %d failed", i);
	}
	printf("concurrent: %d processes, %d copies each\n", NR_SUBMITTERS,
	       SUBMITTER_COPIES);
}

/* A small trace still has to show a wrap, each size and an error */
static void test_short(int fd, uint8_t *buf, const struct dmaeng_info *info)
{
	uint32_t i, n = info->ring_entries + 4;

	for (i = 0; i < n; i++)
		copy_and_verify(fd, buf, 64, i);
	copy_and_verify(fd, buf, 256, 1);
	copy_and_verify(fd, buf, 1024, 2);
	copy_and_verify(fd, buf, COPY_LEN, 3);
	printf("short: %u copies\n", n + 3);
}

/* A fault must report its own error, and the next copy must still work */
static void expect_fault(int fd, uint8_t *buf, uint32_t flag, int want,
			 int moves_data, const char *name)
{
	uint8_t *dst = buf + DST_OFF;
	int before = failures;
	int ret;

	memset(dst, 0xee, 16);
	ret = submit(fd, SRC_OFF, DST_OFF, COPY_LEN, flag);
	CHECK(ret == want, "%s: got %s, want %s", name, strerror(-ret),
	      strerror(-want));
	CHECK(moves_data || dst[0] == 0xee, "%s: data moved", name);
	copy_and_verify(fd, buf, COPY_LEN, flag);
	if (failures == before)
		printf("fault %s: %s, recovered\n", name, strerror(-want));
}

/* The next submit must refuse while a no-wait transfer is still in flight */
static void test_nowait_busy(int fd, uint8_t *buf)
{
	struct dmaeng_xfer x = {
		.src_off = SRC_OFF, .dst_off = DST_OFF, .len = COPY_LEN,
		.flags = DMAENG_XFER_NOWAIT,
	};
	struct dmaeng_xfer second = x;
	int status;

	CHECK(!ioctl(fd, DMAENG_SUBMIT, &x), "submit: %s", strerror(errno));
	if (ioctl(fd, DMAENG_SUBMIT, &second)) {
		CHECK(errno == EBUSY, "got %s, want EBUSY", strerror(errno));
		status = poll_ring(x.desc_idx);
		CHECK(status == 0, "in-flight transfer ended with %d", status);
		printf("no-wait in flight: EBUSY\n");
	} else {
		/* Accepted is only right if the first had really finished */
		CHECK(!(ring_field(x.desc_idx, DMAENG_DESC_OFF_FLAGS) & DMAENG_DESC_OWN),
		      "second submit accepted with the first in flight");
		status = poll_ring(second.desc_idx);
		CHECK(status == 0, "second transfer ended with %d", status);
		printf("no-wait in flight: SKIPPED, transfer finished first (needs a long completion delay)\n");
	}
	copy_and_verify(fd, buf, COPY_LEN, 0x55);
}

static void on_alarm(int sig)
{
	(void)sig;
}

/*
 * A signal during the wait must not restart the ioctl, which would submit
 * twice. It needs the transfer still in flight, so a fast engine skips it.
 */
static void test_signal(int fd, uint8_t *buf)
{
	struct sigaction sa = { .sa_handler = on_alarm };
	struct itimerval it = { .it_value = { .tv_usec = 20000 } };
	int ret;

	CHECK(!sigaction(SIGALRM, &sa, NULL), "sigaction: %s", strerror(errno));
	CHECK(!setitimer(ITIMER_REAL, &it, NULL), "setitimer: %s",
	      strerror(errno));
	ret = submit(fd, SRC_OFF, DST_OFF, COPY_LEN, 0);
	if (ret == 0) {
		printf("signal: SKIPPED, transfer finished first (needs a long completion delay)\n");
		memset(&it, 0, sizeof(it));	/* disarm */
		CHECK(!setitimer(ITIMER_REAL, &it, NULL), "setitimer: %s",
		      strerror(errno));
		return;
	}
	CHECK(ret == -EINTR, "signal: got %s, want EINTR", strerror(-ret));
	copy_and_verify(fd, buf, COPY_LEN, 0x33);
	printf("signal: EINTR, recovered\n");
}

static void test_faults(int fd, uint8_t *buf)
{
	expect_fault(fd, buf, DMAENG_XFER_FAULT_BAD_ADDR, -EFAULT, 0, "bad address");
	expect_fault(fd, buf, DMAENG_XFER_FAULT_RSVD, -EIO, 0, "_rsvd non-zero");
	expect_fault(fd, buf, DMAENG_XFER_FAULT_NO_IRQ, -ETIMEDOUT, 1, "lost interrupt");
	expect_fault(fd, buf, DMAENG_XFER_FAULT_NO_OWN, -EIO, 0, "OWN clear at HEAD");
	expect_fault(fd, buf, DMAENG_XFER_FAULT_RING_ALIGN, -EIO, 0, "misaligned ring");
	expect_fault(fd, buf, DMAENG_XFER_FAULT_RING_ADDR, -EIO, 0, "ring fetch fault");
	test_signal(fd, buf);
	test_nowait_busy(fd, buf);
}

/* Record first and write afterwards: no I/O or statistics in the timed loop */
static void bench_one(int fd, uint8_t *buf, enum path path, uint32_t len,
		      uint32_t n, uint64_t *user_ns, uint64_t *kern_ns, FILE *out)
{
	uint8_t *src = buf + SRC_OFF;
	uint8_t *dst = buf + DST_OFF;
	struct dmaeng_xfer x;
	uint64_t t0, t1;
	uint32_t i;
	int status;

	for (i = 0; i < len; i++)
		src[i] = i * 31 + 1;
	memset(dst, 0, len);

	for (i = 0; i < BENCH_WARMUP + n; i++) {
		memset(&x, 0, sizeof(x));
		x.src_off = SRC_OFF;
		x.dst_off = DST_OFF;
		x.len = len;
		x.flags = path_flags[path];

		t0 = now_ns();
		if (ioctl(fd, DMAENG_SUBMIT, &x)) {
			CHECK(0, "%s len %u: submit %u: %s", path_names[path],
			      len, i, strerror(errno));
			return;
		}
		status = path == PATH_USER_POLL ? poll_ring(x.desc_idx) : 0;
		t1 = now_ns();

		if (status) {
			CHECK(0, "%s len %u: transfer %u: %d", path_names[path],
			      len, i, status);
			return;
		}
		if (i >= BENCH_WARMUP) {
			user_ns[i - BENCH_WARMUP] = t1 - t0;
			kern_ns[i - BENCH_WARMUP] = x.lat_ns;
		}
	}
	CHECK(!memcmp(src, dst, len), "%s len %u: data mismatch",
	      path_names[path], len);

	for (i = 0; i < n; i++)
		fprintf(out, "%s,%u,%llu,%llu\n", path_names[path], len,
			(unsigned long long)user_ns[i],
			(unsigned long long)kern_ns[i]);
	printf("%-11s %6u B: %u samples\n", path_names[path], len, n);
	fflush(stdout);
}

/* Sizes from the spec, section 8.1 */
static void bench(int fd, uint8_t *buf, const char *file, uint32_t n)
{
	static const uint32_t sizes[] = { 64, 256, 1024, 4096, 65536 };
	uint64_t *user_ns = calloc(n, sizeof(*user_ns));
	uint64_t *kern_ns = calloc(n, sizeof(*kern_ns));
	FILE *out = fopen(file, "w");
	cpu_set_t cpus;
	unsigned int s;
	int p;

	if (!user_ns || !kern_ns || !out) {
		CHECK(0, "setup: %s", strerror(errno));
		return;
	}

	/* Fewer migrations and page faults; both are noise here, not signal */
	CPU_ZERO(&cpus);
	CPU_SET(sysconf(_SC_NPROCESSORS_ONLN) - 1, &cpus);
	CHECK(!sched_setaffinity(0, sizeof(cpus), &cpus), "affinity: %s",
	      strerror(errno));
	CHECK(!mlockall(MCL_CURRENT | MCL_FUTURE), "mlockall: %s", strerror(errno));

	fprintf(out, "path,size,user_ns,kernel_ns\n");
	for (p = 0; p < NR_PATHS; p++)
		for (s = 0; s < sizeof(sizes) / sizeof(*sizes); s++)
			bench_one(fd, buf, p, sizes[s], n, user_ns, kern_ns, out);
	fclose(out);
	free(user_ns);
	free(kern_ns);
}

/*
 * An oversized mmap would disclose kernel memory. dma_mmap_coherent() also
 * refuses it, with ENXIO; requiring EINVAL proves the driver's own check,
 * which must not depend on what each architecture's DMA code does.
 *
 * The ring is read-only because a writable one would let userspace aim the
 * engine at any address, which is the whole reason offsets exist.
 */
static void test_mmap_limits(int fd, const struct dmaeng_info *info)
{
	size_t page = sysconf(_SC_PAGESIZE);
	size_t ring_bytes = (info->ring_entries * DMAENG_DESC_SIZE + page - 1) &
			    ~(page - 1);
	off_t ring_off = DMAENG_MMAP_PGOFF_RING * page;
	void *p;

	p = mmap(NULL, info->buf_bytes + page, PROT_READ, MAP_SHARED, fd, 0);
	CHECK(p == MAP_FAILED && errno == EINVAL, "oversized mmap: %s",
	      p == MAP_FAILED ? strerror(errno) : "allowed");
	p = mmap(NULL, page, PROT_READ, MAP_SHARED, fd, 2 * page);
	CHECK(p == MAP_FAILED && errno == EINVAL, "unknown region: %s",
	      p == MAP_FAILED ? strerror(errno) : "allowed");
	p = mmap(NULL, page, PROT_READ, MAP_PRIVATE, fd, 0);
	CHECK(p == MAP_FAILED && errno == EINVAL, "private mmap: %s",
	      p == MAP_FAILED ? strerror(errno) : "allowed");

	p = mmap(NULL, ring_bytes + page, PROT_READ, MAP_SHARED, fd, ring_off);
	CHECK(p == MAP_FAILED && errno == EINVAL, "oversized ring mmap: %s",
	      p == MAP_FAILED ? strerror(errno) : "allowed");
	p = mmap(NULL, page, PROT_READ | PROT_WRITE, MAP_SHARED, fd, ring_off);
	CHECK(p == MAP_FAILED && errno == EPERM, "writable ring mmap: %s",
	      p == MAP_FAILED ? strerror(errno) : "allowed");
	p = mmap(NULL, page, PROT_READ, MAP_SHARED, fd, ring_off);
	CHECK(p != MAP_FAILED, "read-only ring mmap: %s", strerror(errno));
	if (p != MAP_FAILED) {
		CHECK(mprotect(p, page, PROT_READ | PROT_WRITE), "ring made writable");
		munmap(p, page);
	}
	printf("mmap limits enforced\n");
}

int main(int argc, char **argv)
{
	const char *mode = argc > 1 ? argv[1] : "";
	struct dmaeng_info info;
	uint8_t *buf;
	int fd;

	fd = open("/dev/dmaeng", O_RDWR);
	if (fd < 0) {
		perror("open /dev/dmaeng");
		return 1;
	}
	if (ioctl(fd, DMAENG_GET_INFO, &info)) {
		perror("DMAENG_GET_INFO");
		return 1;
	}
	printf("buffer %llu B, ring %u, max_xfer %u\n",
	       (unsigned long long)info.buf_bytes, info.ring_entries,
	       info.max_xfer);

	buf = mmap(NULL, info.buf_bytes, PROT_READ | PROT_WRITE, MAP_SHARED,
		   fd, 0);
	if (buf == MAP_FAILED) {
		perror("mmap");
		return 1;
	}
	ring = mmap(NULL, info.ring_entries * DMAENG_DESC_SIZE, PROT_READ,
		    MAP_SHARED, fd, DMAENG_MMAP_PGOFF_RING * sysconf(_SC_PAGESIZE));
	if (ring == MAP_FAILED) {
		perror("mmap ring");
		return 1;
	}

	if (!strcmp(mode, "--bench") && argc > 2) {
		bench(fd, buf, argv[2],
		      argc > 3 ? (uint32_t)atoi(argv[3]) : BENCH_DEFAULT_N);
	} else if (!strcmp(mode, "--faults")) {
		test_faults(fd, buf);
	} else if (!strcmp(mode, "--short")) {
		test_short(fd, buf, &info);
		test_device_error_recovers(fd, buf, &info);
	} else if (!*mode) {
		test_ring_wrap(fd, buf, &info);
		test_paths(fd, buf, &info);
		test_rejected_by_driver(fd, &info);
		test_device_error_recovers(fd, buf, &info);
		test_nowait_error(fd, buf, &info);
		test_concurrent_submitters(fd, buf);
		test_mmap_limits(fd, &info);
	} else {
		fprintf(stderr, "usage: %s [--short | --faults | --bench FILE [N]]\n",
			argv[0]);
		return 2;
	}

	munmap(buf, info.buf_bytes);
	close(fd);

	if (failures) {
		printf("%d CHECK(S) FAILED\n", failures);
		return 1;
	}
	printf("ALL OK\n");
	return 0;
}
