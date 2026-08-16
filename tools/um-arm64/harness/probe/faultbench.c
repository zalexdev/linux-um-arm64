// SPDX-License-Identifier: GPL-2.0
/*
 * faultbench -- split the cost of a UML guest page fault into its parts.
 *
 * "fault costs 20us against 1.3us native" is not actionable, and worse, the
 * two numbers are not even the same unit: perfbench steps by
 * sysconf(_SC_PAGESIZE), so a 16K guest touches four times the memory per
 * "fault" that a 4K host does. Everything here reports per page AND per
 * megabyte so the comparison survives a change of page size.
 *
 * Each case removes one part of the cost, so the differences attribute it:
 *
 *   touch      fault a fresh anonymous page per iteration. The whole cost:
 *              SIGSEGV to the stub, handoff to the UML kernel, handle_mm_fault
 *              (allocate + zero the page), then one host mmap() per page to
 *              publish it into the stub.
 *   populate   same memory, same mmap flags, but MAP_POPULATE: the guest
 *              kernel builds every PTE inside one syscall and flushes them in
 *              one batch. Same allocation, same zeroing, same host mmap()s --
 *              no per-page interception. touch - populate is what an
 *              intercepted fault costs over and above the work it does.
 *   refault    MADV_DONTNEED then touch again. Same as touch except the VMA
 *              and page tables already exist, so it isolates the setup that
 *              only a first touch pays.
 *   memset     pure userspace write over already-faulted memory. No kernel
 *              entry at all, so this is the floor: no fault can beat the cost
 *              of writing the page's own bytes, and at 16K that floor is four
 *              times what it is at 4K. Subtract it before believing any
 *              per-page overhead number.
 *
 * Run it natively too: every case is portable, and the native numbers say
 * which part of the guest cost is interception and which part is what the
 * hardware charges anyone.
 *
 * Build (guest, static so it runs in any rootfs):
 *   clang --target=aarch64-linux-gnu -static -O2 -o faultbench faultbench.c
 */
#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <time.h>
#include <sys/mman.h>
#include <fcntl.h>

static double now(void)
{
	struct timespec ts;

	clock_gettime(CLOCK_MONOTONIC, &ts);
	return ts.tv_sec + ts.tv_nsec / 1e9;
}

static long pagesz;
static long pages;

/*
 * Minor faults taken by this process, from /proc/self/stat field 10. Reported
 * per page alongside every timing, because a phase that should touch present
 * memory and instead takes one fault per page is a different problem from a
 * phase that is simply slow, and the timing alone cannot tell them apart.
 */
static unsigned long minflt(void)
{
	char buf[1024], *s;
	unsigned long v = 0;
	int fd = open("/proc/self/stat", O_RDONLY);
	ssize_t n;
	int field;

	if (fd < 0)
		return 0;
	n = read(fd, buf, sizeof(buf) - 1);
	close(fd);
	if (n <= 0)
		return 0;
	buf[n] = '\0';

	/*
	 * Field 2 is the comm, is parenthesised, and may itself contain spaces
	 * and ')', so the last ')' in the line ends it -- everything before is
	 * fields 1 and 2. Standing just past it means two fields are consumed,
	 * and each space from here starts the next one: the first space starts
	 * field 3. Starting the count at 3 instead lands on field 9 (num_threads
	 * is 0 for a single-threaded process, which is why the bug read as a
	 * plausible "no faults" rather than as garbage).
	 */
	s = strrchr(buf, ')');
	if (!s)
		return 0;
	s++;
	for (field = 2; *s; s++) {
		if (*s != ' ')
			continue;
		if (++field == 10) {
			v = strtoul(s + 1, NULL, 10);
			break;
		}
	}
	return v;
}

static unsigned long flt_before;

static void mark(void)
{
	flt_before = minflt();
}

static void report(const char *name, double secs)
{
	double mb = (double)pages * pagesz / (1024 * 1024);
	unsigned long faults = minflt() - flt_before;

	printf("%-10s %8.3f us/page  %8.1f MB/s  %6.2f faults/page  (%.3fs)\n",
	       name, secs * 1e6 / pages, mb / secs,
	       (double)faults / pages, secs);
	fflush(stdout);
}

/* Touch one byte per page; volatile so nothing here is optimised away. */
static void touch_all(char *p)
{
	long i;

	for (i = 0; i < pages; i++)
		*(volatile char *)(p + (size_t)i * pagesz) = 1;
}

/*
 * Write every byte with a loop written out here rather than with memset().
 *
 * memset() is chosen by an ifunc from the CPU features libc detects, the
 * guest's HWCAP is deliberately masked down from the host's, and the guest
 * and native builds here are against different libcs entirely (glibc and
 * bionic). Comparing memset across that is comparing two different routines
 * and learning nothing about the guest.
 *
 * This loop is the same instructions on both sides. It is slower than a good
 * memset -- volatile forbids vectorising it -- but it is slower *identically*,
 * which is the only property that matters for the comparison.
 */
static void store_all(char *p, unsigned long v)
{
	volatile unsigned long *q = (volatile unsigned long *)p;
	size_t n = (size_t)pages * pagesz / sizeof(*q);
	size_t i;

	for (i = 0; i < n; i++)
		q[i] = v;
}

static char *fresh(void)
{
	char *p = mmap(NULL, (size_t)pages * pagesz, PROT_READ | PROT_WRITE,
		       MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);

	return p == MAP_FAILED ? NULL : p;
}

int main(int argc, char **argv)
{
	double t0, t1;
	char *p;

	pagesz = sysconf(_SC_PAGESIZE);
	pages = argc > 1 ? atol(argv[1]) : 8192;

	printf("FAULTBENCH_START pagesz=%ld pages=%ld\n", pagesz, pages);

	/* Warm up: first mmap of a process also grows its heap and page tables. */
	p = fresh();
	if (!p)
		return 1;
	touch_all(p);
	munmap(p, (size_t)pages * pagesz);

	/* --- touch: the ordinary case, one intercepted fault per page --- */
	p = fresh();
	if (!p)
		return 1;
	mark();
	t0 = now();
	touch_all(p);
	t1 = now();
	report("touch", t1 - t0);

	/*
	 * --- memset: the floor, no kernel entry (same pages, now present) ---
	 *
	 * Three times, because one pass cannot tell "writing memory is slow
	 * here" from "these pages were not really present and the pass was
	 * paying faults". A first pass that is slow and a second that is not
	 * means the cost belongs to first touch; three equally slow passes
	 * mean the guest is slow at plain stores, which would be a different
	 * and much worse problem.
	 */
	{
		int k;

		for (k = 1; k <= 3; k++) {
			char label[16];

			snprintf(label, sizeof(label), "memset%d", k);
			mark();
			t0 = now();
			memset(p, 2 + k, (size_t)pages * pagesz);
			t1 = now();
			report(label, t1 - t0);

			snprintf(label, sizeof(label), "store%d", k);
			mark();
			t0 = now();
			store_all(p, 0x0101010101010101UL * (unsigned int)k);
			t1 = now();
			report(label, t1 - t0);
		}
	}

	/* --- refault: page tables exist, pages thrown away and touched again --- */
	if (madvise(p, (size_t)pages * pagesz, MADV_DONTNEED) == 0) {
		mark();
		t0 = now();
		touch_all(p);
		t1 = now();
		report("refault", t1 - t0);
	} else {
		printf("refault    MADV_DONTNEED unavailable\n");
	}
	munmap(p, (size_t)pages * pagesz);

	/* --- populate: identical work, zero per-page interception --- */
	mark();
	t0 = now();
	p = mmap(NULL, (size_t)pages * pagesz, PROT_READ | PROT_WRITE,
		 MAP_PRIVATE | MAP_ANONYMOUS | MAP_POPULATE, -1, 0);
	t1 = now();
	if (p == MAP_FAILED)
		printf("populate   MAP_POPULATE failed\n");
	else
		report("populate", t1 - t0);

	/*
	 * Confirm MAP_POPULATE actually populated: if the host ignored it, the
	 * pages are still absent and the number above is meaningless rather
	 * than merely fast. A populated run re-touches at memset speed.
	 */
	if (p != MAP_FAILED) {
		mark();
		t0 = now();
		touch_all(p);
		t1 = now();
		report("post-pop", t1 - t0);
		munmap(p, (size_t)pages * pagesz);
	}

	printf("FAULTBENCH_DONE\n");
	return 0;
}
