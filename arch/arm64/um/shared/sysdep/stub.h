/* SPDX-License-Identifier: GPL-2.0 */
#ifndef __SYSDEP_STUB_H
#define __SYSDEP_STUB_H

#include <stddef.h>
#include <asm/unistd.h>
#include <sys/mman.h>
#include <signal.h>
#include <as-layout.h>
#include <stub-data.h>
#include <sysdep/ptrace_user.h>
#include <generated/asm-offsets.h>

/*
 * The stub is position-independent code mapped into every guest address space.
 * It runs with no libc, no stack guard and no TLS of its own, so every host
 * syscall it makes has to be raw inline asm.
 *
 * aarch64 SVC ABI: x8 = syscall number, x0..x5 = arguments, "svc #0", result in
 * x0. The kernel preserves x1..x30 across the trap and replaces only x0, so
 * unlike x86-64 -- where "syscall" destroys rcx and r11 -- there is nothing to
 * add to the clobber list beyond memory.
 */

#define STUB_MMAP_NR __NR_mmap
/*
 * arm64 needs no syscall to restore guest TLS: TPIDR_EL0 is writable from EL0,
 * so stub_seccomp_restore_state() below is a single "msr" instruction. -1 keeps
 * the seccomp filter's shape identical across subarchs without permitting
 * anything, since no syscall number is -1.
 */
#define STUB_SECCOMP_TLS_SYSCALL (-1)
#define MMAP_OFFSET(o) (o)

static __always_inline long stub_syscall0(long syscall)
{
	register long x8 __asm__("x8") = syscall;
	register long x0 __asm__("x0");

	__asm__ volatile ("svc #0"
		: "=r" (x0)
		: "r" (x8)
		: "memory");

	return x0;
}

static __always_inline long stub_syscall1(long syscall, long arg1)
{
	register long x8 __asm__("x8") = syscall;
	register long x0 __asm__("x0") = arg1;

	__asm__ volatile ("svc #0"
		: "+r" (x0)
		: "r" (x8)
		: "memory");

	return x0;
}

static __always_inline long stub_syscall2(long syscall, long arg1, long arg2)
{
	register long x8 __asm__("x8") = syscall;
	register long x0 __asm__("x0") = arg1;
	register long x1 __asm__("x1") = arg2;

	__asm__ volatile ("svc #0"
		: "+r" (x0)
		: "r" (x8), "r" (x1)
		: "memory");

	return x0;
}

static __always_inline long stub_syscall3(long syscall, long arg1, long arg2,
					  long arg3)
{
	register long x8 __asm__("x8") = syscall;
	register long x0 __asm__("x0") = arg1;
	register long x1 __asm__("x1") = arg2;
	register long x2 __asm__("x2") = arg3;

	__asm__ volatile ("svc #0"
		: "+r" (x0)
		: "r" (x8), "r" (x1), "r" (x2)
		: "memory");

	return x0;
}

static __always_inline long stub_syscall4(long syscall, long arg1, long arg2,
					  long arg3, long arg4)
{
	register long x8 __asm__("x8") = syscall;
	register long x0 __asm__("x0") = arg1;
	register long x1 __asm__("x1") = arg2;
	register long x2 __asm__("x2") = arg3;
	register long x3 __asm__("x3") = arg4;

	__asm__ volatile ("svc #0"
		: "+r" (x0)
		: "r" (x8), "r" (x1), "r" (x2), "r" (x3)
		: "memory");

	return x0;
}

static __always_inline long stub_syscall5(long syscall, long arg1, long arg2,
					  long arg3, long arg4, long arg5)
{
	register long x8 __asm__("x8") = syscall;
	register long x0 __asm__("x0") = arg1;
	register long x1 __asm__("x1") = arg2;
	register long x2 __asm__("x2") = arg3;
	register long x3 __asm__("x3") = arg4;
	register long x4 __asm__("x4") = arg5;

	__asm__ volatile ("svc #0"
		: "+r" (x0)
		: "r" (x8), "r" (x1), "r" (x2), "r" (x3), "r" (x4)
		: "memory");

	return x0;
}

static __always_inline long stub_syscall6(long syscall, long arg1, long arg2,
					  long arg3, long arg4, long arg5,
					  long arg6)
{
	register long x8 __asm__("x8") = syscall;
	register long x0 __asm__("x0") = arg1;
	register long x1 __asm__("x1") = arg2;
	register long x2 __asm__("x2") = arg3;
	register long x3 __asm__("x3") = arg4;
	register long x4 __asm__("x4") = arg5;
	register long x5 __asm__("x5") = arg6;

	__asm__ volatile ("svc #0"
		: "+r" (x0)
		: "r" (x8), "r" (x1), "r" (x2), "r" (x3), "r" (x4), "r" (x5)
		: "memory");

	return x0;
}

/* Deliberate SIGTRAP, so the UML kernel can catch the stub at a known point. */
static __always_inline void trap_myself(void)
{
	__asm__ volatile ("brk #0");
}

/*
 * The stub's data page immediately follows its code page. "adr" gives a
 * PC-relative address in a single instruction, so this is one instruction
 * cheaper than x86's "lea 0(%rip)".
 *
 * The masks are register operands, not immediates: UM_KERN_PAGE_SIZE is not a
 * compile-time constant this code may assume, and a 16K or 64K page host must
 * produce different masks without any source change.
 */
static __always_inline void *get_stub_data(void)
{
	unsigned long ret;

	__asm__ volatile (
		"adr %0, .\n"
		"and %0, %0, %1\n"
		"add %0, %0, %2\n"
		: "=&r" (ret)
		: "r" (~(unsigned long)(UM_KERN_PAGE_SIZE - 1)),
		  "r" ((unsigned long)UM_KERN_PAGE_SIZE));

	return (void *)ret;
}

/*
 * Drop the stack below the stub mapping and call fn. STUB_SIZE is passed in a
 * register rather than as an immediate for the same page-size reason as above.
 */
#define stub_start(fn)							\
	__asm__ volatile (						\
		"sub sp, sp, %0\n"					\
		"blr %1\n"						\
		:: "r" ((unsigned long)STUB_SIZE),			\
		   "r" (&fn)						\
		: "x30", "memory")

/*
 * Reinstate guest TLS after the stub is resumed for a different guest thread.
 * On x86 this costs an arch_prctl(ARCH_SET_FS) syscall; on arm64 TPIDR_EL0 is
 * writable from EL0, so it is a single instruction and no host syscall at all.
 */
static __always_inline void
stub_seccomp_restore_state(struct stub_data_arch *arch)
{
	if (arch->sync & STUB_SYNC_TLS)
		__asm__ volatile ("msr tpidr_el0, %0" :: "r" (arch->tls));

	arch->sync = 0;
}

extern void stub_segv_handler(int, siginfo_t *, void *);
extern void stub_syscall_handler(void);
extern void stub_signal_interrupt(int, siginfo_t *, void *);
extern void stub_signal_restorer(void);

#endif /* __SYSDEP_STUB_H */
