/* SPDX-License-Identifier: GPL-2.0 */
/*
 * Guarded because <stub-futex.h> includes this on top of direct includers.
 * The guard name must differ from __SYSDEP_STUB_H: stub_32.h/stub_64.h below
 * already use that one internally, and defining it here first would silently
 * skip their entire body.
 */
#ifndef __SYSDEP_X86_STUB_H
#define __SYSDEP_X86_STUB_H

#include <asm/unistd.h>
#include <sys/mman.h>
#include <signal.h>
#include <as-layout.h>
#include <stub-data.h>

#ifdef __i386__
#include "stub_32.h"
#else
#include "stub_64.h"
#endif

/*
 * Handoff-word atomics for <stub-futex.h>.
 *
 * x86-TSO already makes every store a release and every load an acquire, so
 * unlike the arm64 versions these only need to add atomicity for the
 * read-modify-writes (the LOCK prefix; XCHG with a memory operand is locked
 * implicitly) and a compiler barrier so the poll re-reads memory.
 */
static __always_inline unsigned int
stub_futex_load_acquire(volatile unsigned int *addr)
{
	unsigned int val = *addr;

	__asm__ volatile("" ::: "memory");

	return val;
}

static __always_inline unsigned int
stub_futex_xchg(volatile unsigned int *addr, unsigned int val)
{
	__asm__ volatile("xchgl %0,%1"
		: "+r" (val), "+m" (*addr)
		:
		: "memory");

	return val;
}

static __always_inline unsigned int
stub_futex_fetch_or(volatile unsigned int *addr, unsigned int bits)
{
	unsigned int old = *addr, prev;

	while (1) {
		unsigned int newval = old | bits;

		__asm__ volatile("lock; cmpxchgl %2,%1"
			: "=a" (prev), "+m" (*addr)
			: "r" (newval), "0" (old)
			: "memory", "cc");
		if (prev == old)
			return prev;
		old = prev;
	}
}

static __always_inline void stub_relax(void)
{
	/* PAUSE, encoded so pre-SSE2 assemblers accept it too. */
	__asm__ volatile("rep; nop" ::: "memory");
}

/*
 * No adaptive spin on x86: it needs a counter with a knowable constant rate
 * readable from the stub, and turning the TSC into one is a CPUID/frequency
 * discovery exercise that freestanding stub code has no business doing. A
 * zero rate makes every spin budget zero, i.e. the pre-spin behavior (park
 * immediately) -- while the waiter-bit wake elision still applies, since that
 * part is timing-free.
 */
static __always_inline unsigned long stub_cycles(void)
{
	return 0;
}

static __always_inline unsigned long stub_cycles_per_us(void)
{
	return 0;
}

extern void stub_segv_handler(int, siginfo_t *, void *);
extern void stub_syscall_handler(void);
extern void stub_signal_interrupt(int, siginfo_t *, void *);
extern void stub_signal_restorer(void);

#endif /* __SYSDEP_X86_STUB_H */
