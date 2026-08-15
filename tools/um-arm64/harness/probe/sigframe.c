// SPDX-License-Identifier: GPL-2.0
/*
 * How big is a signal frame on this host, really?
 *
 * A previous measurement on a cortex-a76 run domain gave ~4.6 KB and that
 * number was written down as if it were a property of aarch64. It is not. It is
 * a property of *this host* and of *what the traced task has executed*:
 * setup_sigframe_layout() in arch/arm64/kernel/signal.c allocates
 * SVE_SIG_CONTEXT_SIZE(vq) whenever the system supports SVE and the task's
 * fp_type is FP_STATE_SVE, plus tpidr2, za, zt and fpmr records on an SME host.
 * With a 2048-bit vector length the SVE record alone is over 8 KB.
 *
 * That matters to UML because the stub's alternate signal stack is a fixed-size
 * member of struct stub_data. The frame the host writes there is sized from the
 * host's features and from what the *guest* has executed inside the stub, and
 * the guest can execute SVE instructions whether or not UML advertised SVE in
 * HWCAP -- HWCAP is advisory, and UML has no EL1 in which to trap the
 * instructions. So the size cannot be assumed at compile time from a
 * measurement taken on one machine.
 *
 * This prints, in order:
 *   - AT_HWCAP/AT_HWCAP2 and whether SVE is present
 *   - AT_MINSIGSTKSZ, the number the kernel itself publishes for this host
 *   - the measured frame size with the task in plain FPSIMD state
 *   - the measured frame size after the task has touched an SVE register
 *
 * The measurement is direct: take a signal on a known alternate stack and
 * subtract the address of the ucontext from the top of that stack.
 *
 * Build:
 *   cc -O2 -o sigframe sigframe.c
 */
#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <signal.h>
#include <sys/auxv.h>
#include <sys/mman.h>
#include <ucontext.h>
#include <setjmp.h>

#ifndef AT_MINSIGSTKSZ
#define AT_MINSIGSTKSZ 51
#endif
#ifndef HWCAP_SVE
#define HWCAP_SVE (1 << 22)
#endif
#ifndef HWCAP2_SME
#define HWCAP2_SME (1 << 23)
#endif

#define ALT_SIZE (1024 * 1024)

static void *alt_base;
static volatile unsigned long frame_bytes;
static volatile unsigned long uc_addr;

static void handler(int sig, siginfo_t *info, void *p)
{
	unsigned long top = (unsigned long)alt_base + ALT_SIZE;

	uc_addr = (unsigned long)p;
	/*
	 * The kernel puts the frame at the top of the alternate stack and grows
	 * down, so the distance from the ucontext to the top is the part of the
	 * frame at or above the ucontext -- which is all of it except the
	 * siginfo that precedes it. Report the whole span for clarity.
	 */
	frame_bytes = top - (unsigned long)p;
}

static unsigned long measure(const char *what)
{
	stack_t st = { .ss_sp = alt_base, .ss_flags = 0, .ss_size = ALT_SIZE };
	struct sigaction sa;

	if (sigaltstack(&st, NULL) < 0) {
		perror("sigaltstack");
		exit(2);
	}

	memset(&sa, 0, sizeof(sa));
	sa.sa_flags = SA_ONSTACK | SA_SIGINFO | SA_NODEFER;
	sa.sa_sigaction = handler;
	sigemptyset(&sa.sa_mask);
	if (sigaction(SIGUSR1, &sa, NULL) < 0) {
		perror("sigaction");
		exit(2);
	}

	frame_bytes = 0;
	raise(SIGUSR1);
	printf("  %-28s ucontext at top-%lu bytes\n", what, frame_bytes);
	return frame_bytes;
}

static sigjmp_buf recover;

static void segv_handler(int sig, siginfo_t *info, void *p)
{
	unsigned long top = (unsigned long)alt_base + ALT_SIZE;

	frame_bytes = top - (unsigned long)p;
	siglongjmp(recover, 1);
}

/*
 * Set FP_STATE_SVE, then take a page fault, with nothing in between that could
 * drop the state. Returns the frame size the kernel actually wrote.
 */
static unsigned long measure_via_fault(void)
{
	stack_t st = { .ss_sp = alt_base, .ss_flags = 0, .ss_size = ALT_SIZE };
	struct sigaction sa;
	volatile unsigned long *bad = (void *)0x10;

	sigaltstack(&st, NULL);
	memset(&sa, 0, sizeof(sa));
	sa.sa_flags = SA_ONSTACK | SA_SIGINFO | SA_NODEFER;
	sa.sa_sigaction = segv_handler;
	sigemptyset(&sa.sa_mask);
	sigaction(SIGSEGV, &sa, NULL);
	sigaction(SIGBUS, &sa, NULL);

	frame_bytes = 0;
	if (sigsetjmp(recover, 1) == 0) {
		__asm__ volatile (
			".arch_extension sve\n"
			"ptrue p0.b\n"
			"mov z0.b, #1\n"
			"mov z1.b, #2\n"
			/* fault now, with SVE state live and no syscall between */
			"str xzr, [%0]\n"
			:: "r" (bad) : "memory", "p0", "z0", "z1");
	}
	printf("  %-28s ucontext at top-%lu bytes\n", "SVE live, async fault",
	       frame_bytes);
	return frame_bytes;
}

int main(void)
{
	unsigned long hwcap = getauxval(AT_HWCAP);
	unsigned long hwcap2 = getauxval(AT_HWCAP2);
	unsigned long minsig = getauxval(AT_MINSIGSTKSZ);
	unsigned long plain, sve;

	alt_base = mmap(NULL, ALT_SIZE, PROT_READ | PROT_WRITE,
			MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
	if (alt_base == MAP_FAILED) {
		perror("mmap");
		return 2;
	}

	printf("AT_HWCAP        = 0x%lx  (SVE %s)\n", hwcap,
	       (hwcap & HWCAP_SVE) ? "PRESENT" : "absent");
	printf("AT_HWCAP2       = 0x%lx  (SME %s)\n", hwcap2,
	       (hwcap2 & HWCAP2_SME) ? "PRESENT" : "absent");
	printf("AT_MINSIGSTKSZ  = %lu%s\n", minsig,
	       minsig ? "" : "  (not published by this kernel)");
	printf("glibc MINSIGSTKSZ = %ld (itself dynamic: sysconf(_SC_SIGSTKSZ))\n", (long)MINSIGSTKSZ);
	printf("sizeof(mcontext_t) = %zu, sizeof(ucontext_t) = %zu\n",
	       sizeof(mcontext_t), sizeof(ucontext_t));
	printf("one page = %ld\n", sysconf(_SC_PAGESIZE));

	printf("\nframe size, FPSIMD state only, signal raised by syscall:\n");
	plain = measure("plain");

	if (hwcap & HWCAP_SVE) {
		/*
		 * The realistic case, and the only one that measures anything:
		 * touch SVE and then fault, with no syscall in between.
		 *
		 * Raising the signal with raise() does not work, because that is
		 * a syscall and arm64 converts the task back to FP_STATE_FPSIMD
		 * on the way in. The stub is never signalled that way -- it takes
		 * an asynchronous SIGSEGV from a page fault in guest code, which
		 * can be mid-SVE. So fault deliberately instead.
		 */
		sve = measure_via_fault();
		printf("\nGROWTH: %lu -> %lu bytes (+%lu, x%.1f)\n",
		       plain, sve, sve - plain, (double)sve / (double)plain);
	} else {
		printf("\n(host has no SVE; rerun on an SVE host to see the growth)\n");
		sve = plain;
	}

	{
		long pg = sysconf(_SC_PAGESIZE);
		unsigned long need = minsig > sve ? minsig : sve;

		printf("\nUML's stub alternate stack is STUB_DATA_PAGES(2) * PAGE_SIZE"
		       " = %ld bytes,\n"
		       "and its sigstack[] member alone is one page = %ld bytes.\n",
		       2 * pg, pg);
		printf("Required here: max(AT_MINSIGSTKSZ %lu, measured frame %lu) = %lu\n",
		       minsig, sve, need);
		printf("VERDICT: %s in sigstack[], %s in the whole stub data area.\n",
		       need <= (unsigned long)pg ? "FITS" : "DOES NOT FIT",
		       need <= 2 * (unsigned long)pg ? "fits" : "DOES NOT FIT");
	}
	return 0;
}
