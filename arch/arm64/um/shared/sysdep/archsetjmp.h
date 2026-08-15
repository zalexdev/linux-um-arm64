/* SPDX-License-Identifier: GPL-2.0 */
/*
 * UML's private jmp_buf for aarch64.
 *
 * UML cannot use libc setjmp()/longjmp() for kernel-side context switching --
 * arch/um/Makefile renames them to kernel_setjmp/kernel_longjmp precisely to
 * keep libc's versions (which may touch the signal mask or a stack guard) out
 * of the way. This is the matching buffer layout; see setjmp_aarch64.S.
 *
 * AAPCS64 callee-saved state: x19-x28, x29 (FP), x30 (LR), SP, and the low
 * 64 bits of v8-v15.
 */
#ifndef _KLIBC_ARCHSETJMP_H
#define _KLIBC_ARCHSETJMP_H

struct __jmp_buf {
	unsigned long __x19;
	unsigned long __x20;
	unsigned long __x21;
	unsigned long __x22;
	unsigned long __x23;
	unsigned long __x24;
	unsigned long __x25;
	unsigned long __x26;
	unsigned long __x27;
	unsigned long __x28;
	unsigned long __x29;	/* frame pointer */
	unsigned long __x30;	/* link register -- resume address */
	unsigned long __sp;
	unsigned long __pad;	/* keep the struct 16-byte aligned */
	unsigned long __d8;
	unsigned long __d9;
	unsigned long __d10;
	unsigned long __d11;
	unsigned long __d12;
	unsigned long __d13;
	unsigned long __d14;
	unsigned long __d15;
};

typedef struct __jmp_buf jmp_buf[1];

unsigned long get_thread_reg(int reg, jmp_buf *buf);

/*
 * get_thread_reg() asks for HOST_PC/HOST_SP of a saved context. There is no
 * separate saved PC: a longjmp resumes at the return address in x30, so that
 * is what "IP" means for a jmp_buf.
 */
#define JB_IP __x30
#define JB_SP __sp

/*
 * Bytes to leave below the top of a fresh kernel stack: none.
 *
 * x86 reserves one word to emulate the return address `call` pushes, so the
 * callee sees the alignment its ABI expects. aarch64 has no pushed return
 * address -- the link register carries it -- and AAPCS64 requires SP to be
 * 16-byte aligned at *all* times, not merely after a prologue. Reserving 8
 * bytes leaves every kernel thread running on a stack that is 8 mod 16, which
 * is not cosmetic: the compiler derives alignment facts from it.
 *
 * Observed symptom: in lib/tests/list-test.c clang computes &entries[0].list as
 * "orr x10, x10, #8" rather than an add, which is valid only because it can
 * prove bit 3 of the address is clear. With a misaligned SP the OR is a no-op,
 * the list entries are built at the wrong offsets, and list_for_each_entry()
 * reads a stack address where a counter should be.
 */
#define ARCH_INIT_SP_RESERVE	0

#endif /* _KLIBC_ARCHSETJMP_H */
