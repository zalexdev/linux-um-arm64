/* SPDX-License-Identifier: GPL-2.0 */
/*
 * Copyright (C) 2007 Jeff Dike (jdike@{addtoit,linux.intel}.com)
 */

#ifndef __START_H__
#define __START_H__

#include <generated/asm-offsets.h>

/*
 * Stolen from linux/const.h, which can't be directly included since
 * this is used in userspace code, which has no access to the kernel
 * headers.  Changed to be suitable for adding casts to the start,
 * rather than "UL" to the end.
 */

/* Some constant macros are used in both assembler and
 * C code.  Therefore we cannot annotate them always with
 * 'UL' and other type specifiers unilaterally.  We
 * use the following macros to deal with this.
 */
#define STUB_START stub_start
#define STUB_CODE STUB_START
#define STUB_DATA (STUB_CODE + UM_KERN_PAGE_SIZE)
/*
 * One page of bookkeeping plus a signal stack. In SECCOMP mode the host kernel
 * writes a complete signal frame onto that stack when it delivers SIGSYS to the
 * stub, so it has to be big enough for the largest frame this host can produce.
 *
 * On x86 one page is comfortable. On arm64 it is not: sizeof(mcontext_t) alone
 * is 4384 bytes, because the architecture reserves __reserved[4096] for the
 * FPSIMD/SVE/SME records, and a measured frame is 4576 bytes -- larger than a
 * 4 KiB page before siginfo is even counted. arm64 publishes the real
 * requirement in AT_MINSIGSTKSZ, which is 4720 on a plain Cortex-A76 and 9984
 * on a host with 512-bit SVE, so the size is a property of the host rather than
 * a constant that can be measured once.
 *
 * Four pages therefore, giving a 12 KiB stack at 4 KiB pages, which covers
 * every AT_MINSIGSTKSZ seen so far with room to spare. At 16 KiB pages a single
 * page is already larger than that, so the layout is unchanged there.
 *
 * A power of two is required, not merely tidy: init_new_context() allocates
 * this with __get_free_pages(..., ilog2(STUB_DATA_PAGES)), so a non-power-of-two
 * would silently allocate fewer pages than the structure occupies.
 *
 * The remaining risk -- a host whose AT_MINSIGSTKSZ exceeds even this -- is not
 * papered over: check_stub_sigstack() compares the two at boot and refuses
 * SECCOMP mode with a message naming both numbers.
 */
#if defined(__aarch64__) && UM_KERN_PAGE_SIZE < 16384
#define STUB_DATA_PAGES 4
#else
#define STUB_DATA_PAGES 2
#endif
#define STUB_SIZE ((1 + STUB_DATA_PAGES) * UM_KERN_PAGE_SIZE)
#define STUB_END (STUB_START + STUB_SIZE)

#ifndef __ASSEMBLER__

#include <sysdep/ptrace.h>

struct task_struct;
extern struct task_struct *cpu_tasks[];

extern unsigned long long physmem_size;

extern unsigned long high_physmem;
extern unsigned long uml_physmem;
extern unsigned long uml_reserved;
extern unsigned long end_vm;
extern unsigned long start_vm;

extern unsigned long brk_start;

extern unsigned long stub_start;

extern int linux_main(int argc, char **argv, char **envp);
extern void uml_finishsetup(void);

struct siginfo;
extern void (*sig_info[])(int, struct siginfo *si, struct uml_pt_regs *, void *);

#endif

#endif
