// SPDX-License-Identifier: GPL-2.0
/*
 * Live terminal view of /dev/dmaeng: the three submission paths interleaved,
 * every copy verified, latency distributions drawn as they fill in.
 *
 *   dmademo [SECONDS] [BYTES]      defaults 15 s, 4096 B
 *
 * For a look, not for numbers: the figures in docs/results.md come from
 * dmatest --bench. Timing is the same (submit to completion, from userspace).
 */
#define _GNU_SOURCE
#include <endian.h>
#include <errno.h>
#include <fcntl.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <time.h>
#include <unistd.h>

#include "dmaeng_regs.h"
#include "dmaeng_uapi.h"

#define SRC_OFF		0ULL
#define DST_OFF		(512ULL * 1024)
#define FINE_NS		100		/* percentile resolution */
#define FINE_BINS	20000		/* up to 2 ms; beyond counts as overflow */
#define HIST_BINS	36		/* log scale, 12 per decade */
#define HIST_MIN_NS	1000		/* 1 us to 1 ms */
#define FRAME_NS	100000000ULL
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

/* Same colours as the plots in docs/imgs */
static const char * const path_rgb[NR_PATHS] = {
	[PATH_BLOCKING] = "42;120;214",
	[PATH_KERNEL_POLL] = "235;104;52",
	[PATH_USER_POLL] = "27;175;122",
};

struct stats {
	uint32_t fine[FINE_BINS + 1];
	uint32_t hist[HIST_BINS];
	uint64_t copies;
	uint64_t verified;
};

static struct stats stats[NR_PATHS];
static uint64_t hist_edge[HIST_BINS + 1];
static uint64_t bad_data, errors;
static const uint8_t *ring;

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

static void record(struct stats *s, uint64_t ns)
{
	uint32_t b = 0;

	s->fine[ns / FINE_NS < FINE_BINS ? ns / FINE_NS : FINE_BINS]++;
	while (b < HIST_BINS - 1 && ns >= hist_edge[b + 1])
		b++;
	s->hist[b]++;
}

static uint64_t percentile(const struct stats *s, uint32_t pct)
{
	uint64_t want = (s->copies * pct + 99) / 100, seen = 0;
	uint32_t i;

	for (i = 0; i <= FINE_BINS; i++) {
		seen += s->fine[i];
		if (seen >= want)
			return (uint64_t)i * FINE_NS + FINE_NS / 2;
	}
	return 0;
}

static void one_copy(int fd, uint8_t *buf, enum path path, uint32_t len,
		     uint8_t seed)
{
	struct dmaeng_xfer x = {
		.src_off = SRC_OFF, .dst_off = DST_OFF, .len = len,
		.flags = path_flags[path],
	};
	uint64_t t0, t1;
	uint32_t i;
	int status;

	for (i = 0; i < len; i++)
		buf[SRC_OFF + i] = seed + i * 31;
	memset(buf + DST_OFF, ~seed, len);

	t0 = now_ns();
	if (ioctl(fd, DMAENG_SUBMIT, &x)) {
		errors++;
		return;
	}
	status = path == PATH_USER_POLL ? poll_ring(x.desc_idx) : 0;
	t1 = now_ns();
	if (status) {
		errors++;
		return;
	}

	stats[path].copies++;
	record(&stats[path], t1 - t0);
	if (memcmp(buf + SRC_OFF, buf + DST_OFF, len))
		bad_data++;
	else
		stats[path].verified++;
}

static void print_us(uint64_t ns)
{
	if (ns >= 100000)
		printf("%6.0f µs", ns / 1000.0);
	else
		printf("%6.1f µs", ns / 1000.0);
}

static void draw(uint32_t len, uint64_t elapsed_ns, int final)
{
	static const char * const level[] = {
		" ", "▁", "▂", "▃", "▄", "▅", "▆", "▇", "█",
	};
	uint64_t total = 0;
	uint32_t peak, b;
	int p;

	printf("\033[H");
	printf(" \033[1mdmaeng\033[0m live demo%*s%5.1f s\033[K\n", 51, "",
	       elapsed_ns / 1e9);
	printf(" \033[2m%s\n %s\033[0m\033[K\n\033[K\n",
	       "Submit-to-completion time seen by userspace. Software path only:",
	       "not DMA hardware latency.");
	printf(" %u B copies, %s\033[K\n\033[K\n", len,
	       "the three paths interleaved, every copy checked byte for byte");
	printf(" %-13s %8s %9s%9s  %-*s\033[K\n", "path", "copies", "p50", "p99",
	       HIST_BINS, "distribution");

	for (p = 0; p < NR_PATHS; p++) {
		const struct stats *s = &stats[p];

		peak = 1;
		for (b = 0; b < HIST_BINS; b++)
			if (s->hist[b] > peak)
				peak = s->hist[b];

		printf(" \033[38;2;%sm■\033[0m %-11s %8llu ", path_rgb[p],
		       path_names[p], (unsigned long long)s->copies);
		print_us(percentile(s, 50));
		print_us(percentile(s, 99));
		printf("  \033[38;2;%sm", path_rgb[p]);
		/* Bins under 1% of the peak stay blank: rare outliers are p99's job */
		for (b = 0; b < HIST_BINS; b++)
			printf("%s", level[s->hist[b] * 100ULL < peak ? 0 :
				1 + (uint64_t)s->hist[b] * 7 / peak]);
		printf("\033[0m\033[K\n");
		total += s->copies;
	}

	/* Axis under the distributions: 12 bins per decade */
	printf(" %43s\033[2m1µs         10µs        100µs       1ms\033[0m\033[K\n"
	       "\033[K\n", "");
	printf(" %llu copies, %.1f MB moved, %llu verified, %llu mismatched, %llu errors\033[K\n",
	       (unsigned long long)total,
	       total * (double)len / 1e6,
	       (unsigned long long)(stats[0].verified + stats[1].verified +
				    stats[2].verified),
	       (unsigned long long)bad_data, (unsigned long long)errors);
	if (final)
		printf("\033[J\n %s\n", bad_data || errors ? "FAILED" : "ALL OK");
	fflush(stdout);
}

int main(int argc, char **argv)
{
	uint64_t seconds = argc > 1 ? strtoull(argv[1], NULL, 0) : 15;
	uint32_t len = argc > 2 ? strtoul(argv[2], NULL, 0) : 4096;
	uint64_t start, next_frame, now;
	struct dmaeng_info info;
	uint8_t seed = 0;
	uint8_t *buf;
	uint32_t b;
	int fd, p;

	fd = open("/dev/dmaeng", O_RDWR);
	if (fd < 0) {
		perror("open /dev/dmaeng");
		return 1;
	}
	if (ioctl(fd, DMAENG_GET_INFO, &info)) {
		perror("DMAENG_GET_INFO");
		return 1;
	}
	if (!len || len > info.max_xfer || DST_OFF + len > info.buf_bytes) {
		fprintf(stderr, "size must be 1 to %u\n", info.max_xfer);
		return 2;
	}
	buf = mmap(NULL, info.buf_bytes, PROT_READ | PROT_WRITE, MAP_SHARED,
		   fd, 0);
	ring = mmap(NULL, info.ring_entries * DMAENG_DESC_SIZE, PROT_READ,
		    MAP_SHARED, fd, DMAENG_MMAP_PGOFF_RING * sysconf(_SC_PAGESIZE));
	if (buf == MAP_FAILED || ring == MAP_FAILED) {
		perror("mmap");
		return 1;
	}

	/* 10^(1/12) per bin */
	hist_edge[0] = HIST_MIN_NS;
	for (b = 1; b <= HIST_BINS; b++)
		hist_edge[b] = hist_edge[b - 1] * 1.2115276586;

	printf("\033[2J\033[?25l");
	start = now_ns();
	next_frame = start;
	for (;;) {
		for (p = 0; p < NR_PATHS; p++)
			one_copy(fd, buf, p, len, seed++);
		now = now_ns();
		if (now - start >= seconds * 1000000000ULL)
			break;
		if (now >= next_frame) {
			draw(len, now - start, 0);
			next_frame = now + FRAME_NS;
		}
	}
	draw(len, now_ns() - start, 1);
	printf("\033[?25h");

	munmap(buf, info.buf_bytes);
	close(fd);
	return bad_data || errors;
}
