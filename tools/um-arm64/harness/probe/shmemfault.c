// SPDX-License-Identifier: GPL-2.0
/*
 * What does one host page fault cost, by mapping kind, on this phone?
 *
 * UML's guest RAM is an unlinked tmpfs file mapped MAP_SHARED into both the UML
 * kernel process and the stub. clear_page() is the first write to a fresh
 * offset, so the UML kernel takes a cold sparse-shmem write fault per HOST page
 * behind every guest page -- four of them per 16K guest page on a 4K host, which
 * harness/probe/childfaults.c has now measured directly at 5.03 host minor
 * faults per guest page (4 in the kernel process, 1 in the stub).
 *
 * Turning that count into microseconds needs the price of one such fault, and
 * the only numbers on record for it were taken on an x86-64 workstation. Rows:
 *
 *   anon_private   MAP_PRIVATE|MAP_ANONYMOUS write fault -- what native pays,
 *                  and what perfbench's own native fault row measures
 *   shmem_cold     first write to a fresh offset of a sparse tmpfs file mapped
 *                  MAP_SHARED -- what the UML kernel pays per host page
 *   shmem_warm     a second MAP_SHARED mapping of an offset already allocated
 *                  and page-cache resident -- what the stub pays
 *   shmem_falloc   fallocate the range first, then take the write fault -- the
 *                  "fallocate ahead of the frontier" idea, priced including the
 *                  fallocate itself
 *
 * TMPDIR picks where the file lives; pass the same directory UML uses.
 *
 * Build:
 *   clang --target=aarch64-linux-gnu -static -O2 -o shmemfault shmemfault.c
 */
#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <time.h>
#include <fcntl.h>
#include <errno.h>
#include <sched.h>
#include <sys/mman.h>
#include <sys/stat.h>

static long pgsz;

static double now(void)
{
	struct timespec ts;

	clock_gettime(CLOCK_MONOTONIC, &ts);
	return ts.tv_sec + ts.tv_nsec / 1e9;
}

static int cmpd(const void *a, const void *b)
{
	double x = *(const double *)a, y = *(const double *)b;

	return x < y ? -1 : x > y ? 1 : 0;
}

static void report(const char *name, double *v, int n)
{
	qsort(v, n, sizeof(*v), cmpd);
	printf("%-14s %8.3f us/page  min %8.3f  max %8.3f  n=%d\n",
	       name, v[n / 2], v[0], v[n - 1], n);
	fflush(stdout);
}

static double bench_anon(long pages)
{
	size_t len = (size_t)pages * pgsz;
	char *p = mmap(NULL, len, PROT_READ | PROT_WRITE,
		       MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
	double t0, t1;
	long i;

	if (p == MAP_FAILED)
		return -1;
	t0 = now();
	for (i = 0; i < pages; i++)
		p[i * pgsz] = 1;
	t1 = now();
	munmap(p, len);
	return (t1 - t0) * 1e6 / pages;
}

/*
 * `round` advances the file offset so every call touches a region that has
 * never been written, which is what makes the fault a cold allocating one.
 */
static double bench_shmem(int fd, long pages, int round, int warm, int falloc)
{
	size_t len = (size_t)pages * pgsz;
	off_t off = (off_t)round * len;
	char *p, *q = NULL;
	double t0, t1, fa = 0;
	long i;

	if (falloc) {
		double f0 = now();

		if (fallocate(fd, 0, off, len))
			return -1;
		fa = now() - f0;
	}

	p = mmap(NULL, len, PROT_READ | PROT_WRITE, MAP_SHARED, fd, off);
	if (p == MAP_FAILED)
		return -1;

	if (warm) {
		/* Allocate and populate through a first mapping, then measure a
		 * second one: the page cache is warm, only the PTE is missing. */
		for (i = 0; i < pages; i++)
			p[i * pgsz] = 1;
		q = mmap(NULL, len, PROT_READ | PROT_WRITE, MAP_SHARED, fd, off);
		if (q == MAP_FAILED) {
			munmap(p, len);
			return -1;
		}
	}

	t0 = now();
	for (i = 0; i < pages; i++)
		(warm ? q : p)[i * pgsz] = 2;
	t1 = now();

	munmap(p, len);
	if (q)
		munmap(q, len);
	return ((t1 - t0) + fa) * 1e6 / pages;
}

int main(int argc, char **argv)
{
	long pages = argc > 1 ? atol(argv[1]) : 8192;
	int rounds = argc > 2 ? atoi(argv[2]) : 7;
	const char *dir = getenv("TMPDIR") ? getenv("TMPDIR") : "/tmp";
	char path[256];
	double v[64];
	int fd, r;
	cpu_set_t s;

	pgsz = sysconf(_SC_PAGESIZE);
	if (rounds > 64)
		rounds = 64;

	CPU_ZERO(&s);
	CPU_SET(4, &s);
	sched_setaffinity(0, sizeof(s), &s);

	snprintf(path, sizeof(path), "%s/shmemfault-XXXXXX", dir);
	fd = mkstemp(path);
	if (fd < 0) {
		perror("mkstemp");
		return 1;
	}
	unlink(path);
	/* Room for every round of every test at a distinct offset. */
	if (ftruncate(fd, (off_t)pages * pgsz * (rounds + 2) * 4)) {
		perror("ftruncate");
		return 1;
	}

	printf("SHMEMFAULT_START pagesize=%ld pages=%ld dir=%s\n",
	       pgsz, pages, dir);

	for (r = 0; r < rounds; r++)
		v[r] = bench_anon(pages);
	report("anon_private", v, rounds);

	for (r = 0; r < rounds; r++)
		v[r] = bench_shmem(fd, pages, r, 0, 0);
	report("shmem_cold", v, rounds);

	for (r = 0; r < rounds; r++)
		v[r] = bench_shmem(fd, pages, rounds + 1 + r, 1, 0);
	report("shmem_warm", v, rounds);

	for (r = 0; r < rounds; r++)
		v[r] = bench_shmem(fd, pages, 2 * rounds + 2 + r, 0, 1);
	report("shmem_falloc", v, rounds);

	printf("SHMEMFAULT_DONE\n");
	return 0;
}
