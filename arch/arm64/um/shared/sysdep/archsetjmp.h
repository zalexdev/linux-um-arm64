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

#endif /* _KLIBC_ARCHSETJMP_H */
