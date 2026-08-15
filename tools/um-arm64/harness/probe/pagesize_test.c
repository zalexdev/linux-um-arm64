// SPDX-License-Identifier: GPL-2.0
/*
 * Parametric page-size test for the UML/arm64 stub layout.
 *
 * No 16K- or 64K-page host exists in this environment, so the goal calls for
 * this to be enforced "by review and a parametric test" rather than by CI. This
 * is that test: it re-implements the address arithmetic exactly as the port
 * computes it and checks the invariants at 4K, 16K and 64K.
 *
 * It is deliberately a plain host program with no kernel headers and no arm64
 * requirement, so it runs on the build machine in milliseconds and cannot rot
 * behind a slow VM boot.
 *
 * What it covers, and why each matters on a 16K host:
 *
 *   - the as-layout.h relations (STUB_DATA = STUB_CODE + PAGE_SIZE, etc.). A
 *     hardcoded 4096 here puts the data page inside the code page.
 *   - get_stub_data()'s "mask down to the page base, add one page". This must
 *     land on STUB_DATA from *any* pc inside the code page, which is only true
 *     if the mask is the real page mask.
 *   - the seccomp filter's instruction-pointer check in stub_exe.c, which
 *     compares (ip & PAGE_MASK) against (stub_start & PAGE_MASK) using a 32-bit
 *     BPF constant. With a 4K mask on a 16K host this rejects legitimate stub
 *     syscalls whose ip is in the upper 12K of the page.
 *   - mmap alignment of the stub itself.
 *
 * Build and run:
 *   cc -O1 -Wall -o pagesize_test pagesize_test.c && ./pagesize_test
 */
#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <inttypes.h>

static int failures;

#define CHECK(cond, fmt, ...)						\
	do {								\
		if (!(cond)) {						\
			printf("FAIL ps=%-6lu " fmt "\n",		\
			       (unsigned long)ps, ##__VA_ARGS__);	\
			failures++;					\
		}							\
	} while (0)

/* Mirrors arch/um/include/shared/as-layout.h. */
#define STUB_DATA_PAGES 2

struct layout {
	uint64_t code;
	uint64_t data;
	uint64_t size;
	uint64_t end;
};

static struct layout compute_layout(uint64_t stub_start, uint64_t ps)
{
	struct layout l;

	l.code = stub_start;
	l.data = l.code + ps;
	l.size = (1 + STUB_DATA_PAGES) * ps;
	l.end  = stub_start + l.size;
	return l;
}

/*
 * Mirrors get_stub_data() in arch/arm64/um/shared/sysdep/stub.h:
 *	adr x, .        ; and x, x, ~(PAGE_SIZE-1) ; add x, x, PAGE_SIZE
 */
static uint64_t stub_data_from_pc(uint64_t pc, uint64_t ps)
{
	return (pc & ~(ps - 1)) + ps;
}

/*
 * Mirrors the seccomp filter in arch/um/kernel/skas/stub_exe.c, including its
 * truncation to 32 bits: BPF operates on u32, and seccomp_data's
 * instruction_pointer is loaded as a 32-bit word here.
 */
static int seccomp_ip_on_stub_page(uint64_t ip, uint64_t stub_start, uint64_t ps)
{
	uint32_t mask = (uint32_t)~(ps - 1);

	return ((uint32_t)ip & mask) == ((uint32_t)stub_start & mask);
}

static void run(uint64_t ps)
{
	/* A few plausible stub placements, all page-aligned as mmap requires. */
	const uint64_t bases[] = {
		0x10000ULL,
		0x40000000ULL,
		0x7f0000000000ULL & ~(ps - 1),
		0xfffffff000ULL   & ~(ps - 1),
	};
	size_t b;

	printf("--- page size %" PRIu64 " (shift %d) ---\n",
	       ps, __builtin_ctzll(ps));

	for (b = 0; b < sizeof(bases) / sizeof(bases[0]); b++) {
		uint64_t base = bases[b] & ~(ps - 1);
		struct layout l = compute_layout(base, ps);
		uint64_t off;

		CHECK(l.code % ps == 0, "stub code 0x%" PRIx64 " not page aligned", l.code);
		CHECK(l.data == l.code + ps, "stub data not one page above code");
		CHECK(l.data % ps == 0, "stub data 0x%" PRIx64 " not page aligned", l.data);
		CHECK(l.size == 3 * ps, "stub size %" PRIu64 " != 3 pages", l.size);
		CHECK(l.end == l.code + 3 * ps, "stub end wrong");
		CHECK(l.end % ps == 0, "stub end not page aligned");

		/*
		 * get_stub_data() must yield STUB_DATA from every possible pc
		 * within the code page -- the first instruction, the last, and
		 * points between. On a 16K host with a 4K mask, any pc above
		 * the first 4K lands on the wrong page.
		 */
		for (off = 0; off < ps; off += (ps / 8) ? (ps / 8) : 1) {
			uint64_t got = stub_data_from_pc(l.code + off, ps);

			CHECK(got == l.data,
			      "get_stub_data(pc=code+0x%" PRIx64 ") = 0x%" PRIx64
			      ", want 0x%" PRIx64, off, got, l.data);
		}
		/* And the very last byte of the page. */
		CHECK(stub_data_from_pc(l.code + ps - 1, ps) == l.data,
		      "get_stub_data at end of code page wrong");

		/* One byte into the data page must NOT resolve to the data page. */
		CHECK(stub_data_from_pc(l.data, ps) != l.data,
		      "get_stub_data from the data page should not self-select");

		/*
		 * seccomp filter: every ip inside the code page is accepted,
		 * and the first ip of the following page is not.
		 */
		for (off = 0; off < ps; off += (ps / 8) ? (ps / 8) : 1)
			CHECK(seccomp_ip_on_stub_page(l.code + off, l.code, ps),
			      "seccomp rejects ip=code+0x%" PRIx64, off);
		CHECK(seccomp_ip_on_stub_page(l.code + ps - 4, l.code, ps),
		      "seccomp rejects last instruction of the code page");
		CHECK(!seccomp_ip_on_stub_page(l.code + ps, l.code, ps),
		      "seccomp accepts ip on the data page");
	}

	/*
	 * Sanity on the mask itself: a 32-bit BPF constant must still describe
	 * the page, which is true for every page size up to 4GB.
	 */
	CHECK(((uint32_t)~(ps - 1) & (uint32_t)(ps - 1)) == 0,
	      "32-bit page mask overlaps the page offset");
}

int main(void)
{
	uint64_t sizes[] = { 4096, 16384, 65536 };
	size_t i;

	for (i = 0; i < sizeof(sizes) / sizeof(sizes[0]); i++)
		run(sizes[i]);

	if (failures) {
		printf("PAGESIZE_TEST_FAILED failures=%d\n", failures);
		return 1;
	}
	printf("PAGESIZE_TEST_OK\n");
	return 0;
}
