/* SPDX-License-Identifier: GPL-2.0 */
/*
 * Fault information for ARCH=um SUBARCH=arm64.
 */
#ifndef __FAULTINFO_ARM64_H
#define __FAULTINFO_ARM64_H

/*
 * ESR_EL1 fields we care about. The signal frame delivers ESR in an
 * esr_context record inside mcontext.__reserved[]; see os-Linux/mcontext.c.
 *
 * EC 0x20/0x21 are instruction aborts (from a lower EL / from the same EL),
 * EC 0x24/0x25 are data aborts. For a data abort, WnR (bit 6) says whether the
 * access was a write. This is the arm64 equivalent of x86's page-fault error
 * code bit 1, and it is what makes copy-on-write distinguishable from a plain
 * read fault.
 */
#define UM_ESR_EC_SHIFT		26
#define UM_ESR_EC_MASK		0x3f
#define UM_ESR_EC(esr)		(((esr) >> UM_ESR_EC_SHIFT) & UM_ESR_EC_MASK)
#define UM_ESR_EC_IABT_LOW	0x20
#define UM_ESR_EC_IABT_CUR	0x21
#define UM_ESR_EC_DABT_LOW	0x24
#define UM_ESR_EC_DABT_CUR	0x25
#define UM_ESR_WNR		(1UL << 6)

struct faultinfo {
	int is_write;
	unsigned long addr;
	unsigned int esr;
};

#define FAULT_WRITE(fi) ((fi).is_write)
#define FAULT_ADDRESS(fi) ((fi).addr)
/*
 * The architecture's own description of why the fault happened. On x86 this is
 * the page-fault error code; on arm64 the ESR, which encodes considerably more
 * (exception class, fault status code, write-not-read) and is what an arm64
 * developer reading a segfault report will expect to see.
 */
#define FAULT_ERROR_CODE(fi) ((fi).esr)

/*
 * "Fixable" means the fault is a translation/permission problem UML can resolve
 * by populating the guest page tables, rather than something structural. Both
 * instruction and data aborts qualify: a guest jumping into a not-yet-faulted-in
 * page is as ordinary as a guest loading from one.
 */
#define SEGV_IS_FIXABLE(fi)						\
	(UM_ESR_EC((fi)->esr) == UM_ESR_EC_DABT_LOW ||			\
	 UM_ESR_EC((fi)->esr) == UM_ESR_EC_DABT_CUR ||			\
	 UM_ESR_EC((fi)->esr) == UM_ESR_EC_IABT_LOW ||			\
	 UM_ESR_EC((fi)->esr) == UM_ESR_EC_IABT_CUR)

#define PTRACE_FULL_FAULTINFO 1

/*
 * Record a landing pad for a fault taken inside __get_kernel_nofault /
 * __put_kernel_nofault. The SIGSEGV handler in arch/um/kernel/trap.c redirects
 * the mcontext PC to thread.segv_continue, so control resumes at label 1 with
 * _faulted set.
 *
 * Local numeric labels are used rather than x86's "%=" name mangling: the
 * aarch64 assembler resolves 1f/1b per-instance, so nothing has to be uniquified
 * and the macro stays safe when inlined more than once in a function.
 */
#define ___backtrack_faulted(_faulted)					\
	do {								\
		unsigned long __tmp;					\
									\
		asm volatile (						\
			"adr %2, 1f\n"					\
			"str %2, %1\n"					\
			"mov %w0, #0\n"					\
			"b 2f\n"					\
			"1:\n"						\
			"mov %w0, #1\n"					\
			"2:"						\
			: "=&r" (_faulted),				\
			  "=m" (current->thread.segv_continue),		\
			  "=&r" (__tmp) ::				\
		);							\
	} while (0)

#endif /* __FAULTINFO_ARM64_H */
