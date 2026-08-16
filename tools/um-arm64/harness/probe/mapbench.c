// SPDX-License-Identifier: GPL-2.0
/*
 * mapbench -- what a guest page fault costs the host, measured on the host.
 *
 * UML maps guest memory into the stub one PTE at a time: arch/um/kernel/tlb.c
 * calls ops->mmap(addr, PAGE_SIZE, ...) per page, and os-Linux/skas/mem.c
 * map() folds a page into the previous request only when its offset in the
 * physmem file continues that request exactly:
 *
 *	sc->mem.offset == MMAP_OFFSET(offset - sc->mem.length)
 *
 * Guest anonymous memory is filled by the page allocator, which hands out
 * whatever order-0 frame it likes, so consecutive guest virtual pages sit at
 * unrelated physmem offsets and that test almost always fails. One host
 * mmap() per guest page.
 *
 * This measures the difference the merge makes, on the host, with no guest
 * involved -- so the number cannot be explained away by anything in UML:
 *
 *   scattered  offsets shuffled: one mmap() per page, what UML does today
 *   linear     offsets in order: pages fold into one call, what UML would do
 *              if the frames behind a fault were physically contiguous
 *
 * Both walk the same virtual addresses, map the same bytes, from the same fd,
 * and differ only in the file offset each page comes from.
 *
 * The other half of the cost is address-space shape. Scattered mapping leaves
 * every page its own VMA, because adjacent VMAs only merge when their file
 * offsets line up -- the same condition. So the scattered case also pays a
 * growing maple-tree, and "vmas" reports what each case left behind.
 *
 * Build:
 *   $NDK/toolchains/llvm/prebuilt/linux-x86_64/bin/clang \
 *       --target=aarch64-linux-android30 -O2 -static -o mapbench mapbench.c
 */
#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <time.h>
#include <fcntl.h>
#include <sys/mman.h>
#include <sys/syscall.h>

static double now(void)
{
	struct timespec ts;

	clock_gettime(CLOCK_MONOTONIC, &ts);
	return ts.tv_sec + ts.tv_nsec / 1e9;
}

/* Count VMAs the cheap way; /proc/self/maps lines == VMAs. */
static long count_vmas(void)
{
	char buf[65536];
	long n = 0;
	int fd = open("/proc/self/maps", O_RDONLY);
	ssize_t r;

	if (fd < 0)
		return -1;
	while ((r = read(fd, buf, sizeof(buf))) > 0)
		for (ssize_t i = 0; i < r; i++)
			if (buf[i] == '\n')
				n++;
	close(fd);
	return n;
}

/*
 * A xorshift so the shuffle is reproducible across runs and configurations:
 * the two cases must differ in offset order and in nothing else, including
 * which pages the allocator happened to touch first.
 */
static unsigned long rnd_state = 88172645463325252UL;

static unsigned long rnd(void)
{
	rnd_state ^= rnd_state << 13;
	rnd_state ^= rnd_state >> 7;
	rnd_state ^= rnd_state << 17;
	return rnd_state;
}

static void run(const char *name, int fd, char *base, long pages, long pagesz,
		int shuffle, int touch)
{
	long *off = malloc(pages * sizeof(*off));
	double t0, t1;
	long vma0, vma1;
	long i;

	if (!off)
		return;
	for (i = 0; i < pages; i++)
		off[i] = i;
	if (shuffle) {
		for (i = pages - 1; i > 0; i--) {
			long j = rnd() % (i + 1);
			long t = off[i]; off[i] = off[j]; off[j] = t;
		}
	}

	/* Start from a clean slate: one PROT_NONE VMA over the whole range. */
	mmap(base, (size_t)pages * pagesz, PROT_NONE,
	     MAP_PRIVATE | MAP_ANONYMOUS | MAP_FIXED, -1, 0);
	vma0 = count_vmas();

	t0 = now();
	for (i = 0; i < pages; i++) {
		void *p = mmap(base + (size_t)i * pagesz, pagesz,
			       PROT_READ | PROT_WRITE,
			       MAP_SHARED | MAP_FIXED, fd,
			       (off_t)off[i] * pagesz);

		if (p == MAP_FAILED) {
			printf("%-10s mmap failed at %ld\n", name, i);
			free(off);
			return;
		}
		if (touch)
			*(volatile char *)p = 1;
	}
	t1 = now();
	vma1 = count_vmas();

	printf("%-10s %8.3f us/page   (%ld pages in %.3fs)   vmas %ld -> %ld\n",
	       name, (t1 - t0) * 1e6 / pages, pages, t1 - t0, vma0, vma1);

	/*
	 * Now write every byte of what was just mapped. No mmap, no fault, no
	 * kernel entry -- only stores through whatever page tables the mapping
	 * left behind. Both cases cover the same bytes of the same memfd, so a
	 * difference here is the shape of the mapping and nothing else: it is
	 * the part of the guest's cost that no amount of fault-path tuning can
	 * reach.
	 */
	{
		double m0, m1, s0, s1;
		double mb = (double)pages * pagesz / (1024 * 1024);
		volatile unsigned long *q = (volatile unsigned long *)base;
		size_t words = (size_t)pages * pagesz / sizeof(*q);
		size_t k;

		memset(base, 1, (size_t)pages * pagesz);	/* fault them in */
		m0 = now();
		memset(base, 2, (size_t)pages * pagesz);
		m1 = now();

		/*
		 * The same plain store loop the guest benchmark uses. memset()
		 * alone hides this effect: a tuned memset reaches for
		 * non-temporal stores, which bypass the caches and are
		 * therefore indifferent to how many mappings the region is
		 * split across. Ordinary stores are not, and ordinary stores
		 * are what programs actually do.
		 */
		s0 = now();
		for (k = 0; k < words; k++)
			q[k] = 3;
		s1 = now();

		printf("%-10s %8.1f MB/s memset   %8.1f MB/s plain stores\n",
		       "", mb / (m1 - m0), mb / (s1 - s0));
	}
	free(off);
}

int main(int argc, char **argv)
{
	long pagesz = argc > 2 ? atol(argv[2]) : 16384;
	long pages = argc > 1 ? atol(argv[1]) : 8192;
	size_t len = (size_t)pages * pagesz;
	char *base;
	int fd;

	/*
	 * memfd, not a real file: UML's physmem is a memfd too, so page cache
	 * behaviour and the writeback path match, and nothing here touches
	 * storage.
	 */
	fd = syscall(__NR_memfd_create, "physmem", 0);
	if (fd < 0) {
		perror("memfd_create");
		return 1;
	}
	if (ftruncate(fd, len) < 0) {
		perror("ftruncate");
		return 1;
	}

	/* Reserve the window once; every case re-maps inside it. */
	base = mmap(NULL, len, PROT_NONE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
	if (base == MAP_FAILED) {
		perror("reserve");
		return 1;
	}

	printf("MAPBENCH_START pagesz=%ld pages=%ld\n", pagesz, pages);
	/*
	 * The cost UML pays that a native process does not.
	 *
	 * When UML resolves a guest fault it zeroes the page through the
	 * kernel's own mapping of physmem and then publishes it to the stub
	 * with mmap(). The page is resident by then -- but the stub's page
	 * tables are still empty for it, so the guest's very next access takes
	 * a second fault, this one on the host, invisible to the guest (it
	 * shows up in no guest counter). These four cases price that, and
	 * price the fix: MAP_POPULATE fills the page tables inside the mmap()
	 * that was happening anyway.
	 *
	 *   map           mmap() alone, page never touched
	 *   map+touch     mmap() then one byte -- what the stub does today
	 *   pop           mmap(MAP_POPULATE) alone
	 *   pop+touch     mmap(MAP_POPULATE) then one byte -- the proposal
	 *
	 * If (map+touch) - (map) is real and (pop+touch) ~= (pop), the second
	 * fault is worth removing and MAP_POPULATE removes it.
	 */
	{
		double t0, t1;
		long i;
		long *off = malloc(pages * sizeof(*off));
		int c;

		if (!off)
			return 1;
		static const struct { const char *name; int pop, touch; } cases[] = {
			{ "map",       0, 0 }, { "map+touch", 0, 1 },
			{ "pop",       1, 0 }, { "pop+touch", 1, 1 },
			{ "map2",      0, 0 }, { "map+touch2", 0, 1 },
			{ "pop2",      1, 0 }, { "pop+touch2", 1, 1 },
		};

		/* Make every page resident first, as UML's zeroing would. */
		mmap(base, len, PROT_READ | PROT_WRITE, MAP_SHARED | MAP_FIXED,
		     fd, 0);
		memset(base, 1, len);

		for (c = 0; c < (int)(sizeof(cases) / sizeof(cases[0])); c++) {
			int flags = MAP_SHARED | MAP_FIXED;

			if (cases[c].pop)
				flags |= MAP_POPULATE;

			/* Drop the mappings without dropping residency. */
			mmap(base, len, PROT_NONE,
			     MAP_PRIVATE | MAP_ANONYMOUS | MAP_FIXED, -1, 0);

			t0 = now();
			for (i = 0; i < pages; i++) {
				void *p = mmap(base + (size_t)i * pagesz,
					       pagesz, PROT_READ | PROT_WRITE,
					       flags, fd,
					       (off_t)i * pagesz);

				if (p == MAP_FAILED)
					break;
				if (cases[c].touch)
					*(volatile char *)p = 2;
			}
			t1 = now();
			printf("%-12s %8.3f us/page   (%ld pages in %.3fs)\n",
			       cases[c].name, (t1 - t0) * 1e6 / pages, pages,
			       t1 - t0);
		}

		/*
		 * The case above interleaves map and touch, so when a page is
		 * touched the VMA ends right there and the host has nothing
		 * ahead to work with. UML does not behave that way: the stub
		 * executes a whole batch of mmaps and only then returns to the
		 * guest, which touches the pages afterwards.
		 *
		 * That distinction is the whole question for merging. Linux
		 * maps up to fault_around_bytes of neighbours on a file-backed
		 * fault, but only within one VMA -- so a merged mapping gets
		 * one host fault per ~16 pages, and a per-page mapping gets one
		 * per page and cannot ever do better.
		 *
		 * Map everything first, then walk it. The difference between
		 * these two is what UML would gain by making the frames behind
		 * a fault physically contiguous.
		 */
		for (c = 0; c < 4; c++) {
			int shuf = c & 1;
			long vb, va;

			mmap(base, len, PROT_NONE,
			     MAP_PRIVATE | MAP_ANONYMOUS | MAP_FIXED, -1, 0);
			for (i = 0; i < pages; i++)
				off[i] = i;
			if (shuf)
				for (i = pages - 1; i > 0; i--) {
					long j = rnd() % (i + 1);
					long t = off[i]; off[i] = off[j]; off[j] = t;
				}
			for (i = 0; i < pages; i++)
				mmap(base + (size_t)i * pagesz, pagesz,
				     PROT_READ | PROT_WRITE,
				     MAP_SHARED | MAP_FIXED, fd,
				     (off_t)off[i] * pagesz);
			vb = count_vmas();

			t0 = now();
			for (i = 0; i < pages; i++)
				*(volatile char *)(base + (size_t)i * pagesz) = 3;
			t1 = now();
			va = count_vmas();
			printf("%-12s %8.3f us/page   touch-after-map-all, vmas %ld (%s)\n",
			       shuf ? "walk/split" : "walk/merged",
			       (t1 - t0) * 1e6 / pages, va,
			       vb == va ? "stable" : "changed");
		}
	}
	/*
	 * Warm the file's pages so the timed loops are not measuring writeback
	 * setup or first-touch allocation of the memfd itself.
	 */
	run("warmup", fd, base, pages, pagesz, 0, 1);
	run("linear", fd, base, pages, pagesz, 0, 0);
	run("scattered", fd, base, pages, pagesz, 1, 0);
	run("linear2", fd, base, pages, pagesz, 0, 0);
	run("scatter2", fd, base, pages, pagesz, 1, 0);
	printf("MAPBENCH_DONE\n");
	return 0;
}
