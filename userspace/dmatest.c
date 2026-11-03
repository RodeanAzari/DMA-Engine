// SPDX-License-Identifier: GPL-2.0
/*
 * End-to-end check of /dev/dmaeng. Exits non-zero on any failure.
 * Latency measurement and the other submission paths come on Day 6.
 *
 *   dmatest           the full suite
 *   dmatest --short   small workload, for a trace file worth committing
 *   dmatest --faults  error paths; needs a FAULT_INJECT=1 driver build
 */
#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <sys/time.h>
#include <sys/wait.h>
#include <unistd.h>

#include "dmaeng_uapi.h"

#define SRC_OFF		0ULL
#define DST_OFF		(128ULL * 1024)
#define COPY_LEN	4096

#define NR_SUBMITTERS	4
#define SUBMITTER_COPIES 200
#define SUBMITTER_STRIDE (256ULL * 1024)

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

/* One verified copy. The guard byte catches a device that overruns len. */
static void copy_at(int fd, uint8_t *buf, uint64_t src_off, uint64_t dst_off,
		    uint32_t len, uint8_t seed)
{
	uint8_t *src = buf + src_off;
	uint8_t *dst = buf + dst_off;
	struct dmaeng_xfer x = {
		.src_off = src_off, .dst_off = dst_off, .len = len,
	};
	uint8_t want;
	uint32_t i;

	/* Position-dependent, so a stuck or shifted copy cannot pass */
	for (i = 0; i < len; i++)
		src[i] = seed + i * 31;
	memset(dst, ~seed, len + 1);
	want = ~seed;

	if (ioctl(fd, DMAENG_SUBMIT, &x)) {
		CHECK(0, "len %u: submit: %s", len, strerror(errno));
		return;
	}
	CHECK(x.lat_ns > 0, "len %u: lat_ns not filled", len);
	CHECK(!memcmp(src, dst, len), "len %u: data mismatch", len);
	CHECK(dst[len] == want, "len %u: wrote past the end", len);
}

static void copy_and_verify(int fd, uint8_t *buf, uint32_t len, uint8_t seed)
{
	copy_at(fd, buf, SRC_OFF, DST_OFF, len, seed);
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
			copy_at(fd, buf, src, src + DST_OFF, COPY_LEN, c + i);
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
	int ret;

	memset(dst, 0xee, 16);
	ret = submit(fd, SRC_OFF, DST_OFF, COPY_LEN, flag);
	CHECK(ret == want, "%s: got %s, want %s", name, strerror(-ret),
	      strerror(-want));
	CHECK(moves_data || dst[0] == 0xee, "%s: data moved", name);
	copy_and_verify(fd, buf, COPY_LEN, flag);
	printf("fault %s: %s, recovered\n", name, strerror(-want));
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
	test_signal(fd, buf);
}

/*
 * An oversized mmap would disclose kernel memory. dma_mmap_coherent() also
 * refuses it, with ENXIO; requiring EINVAL proves the driver's own check,
 * which must not depend on what each architecture's DMA code does.
 */
static void test_mmap_limits(int fd, const struct dmaeng_info *info)
{
	size_t page = sysconf(_SC_PAGESIZE);
	void *p;

	p = mmap(NULL, info->buf_bytes + page, PROT_READ, MAP_SHARED, fd, 0);
	CHECK(p == MAP_FAILED && errno == EINVAL, "oversized mmap: %s",
	      p == MAP_FAILED ? strerror(errno) : "allowed");
	p = mmap(NULL, page, PROT_READ, MAP_SHARED, fd, page);
	CHECK(p == MAP_FAILED && errno == EINVAL, "offset mmap: %s",
	      p == MAP_FAILED ? strerror(errno) : "allowed");
	p = mmap(NULL, page, PROT_READ, MAP_PRIVATE, fd, 0);
	CHECK(p == MAP_FAILED && errno == EINVAL, "private mmap: %s",
	      p == MAP_FAILED ? strerror(errno) : "allowed");
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

	if (!strcmp(mode, "--faults")) {
		test_faults(fd, buf);
	} else if (!strcmp(mode, "--short")) {
		test_short(fd, buf, &info);
		test_device_error_recovers(fd, buf, &info);
	} else if (!*mode) {
		test_ring_wrap(fd, buf, &info);
		test_rejected_by_driver(fd, &info);
		test_device_error_recovers(fd, buf, &info);
		test_concurrent_submitters(fd, buf);
		test_mmap_limits(fd, &info);
	} else {
		fprintf(stderr, "usage: %s [--short | --faults]\n", argv[0]);
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
