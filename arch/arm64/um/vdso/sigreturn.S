/* SPDX-License-Identifier: GPL-2.0 */
/*
 * Signal-return trampoline for the UML/arm64 vDSO.
 *
 * arm64 userspace cannot return from a signal handler by itself: unlike i386,
 * there is nowhere to put a trampoline in the signal frame, because the stack is
 * not executable. glibc and musl on aarch64 therefore do *not* set SA_RESTORER
 * and rely on the kernel pointing x30 at this symbol. Without it, every guest
 * signal handler would return to a wild address.
 *
 * The CFI directives are what lets gdb and libunwind walk out of a signal
 * handler and back into the interrupted frame. They mirror
 * arch/arm64/kernel/vdso/sigreturn.S: the frame base is sp, and the saved
 * registers live inside the sigcontext at a fixed offset from it.
 */

#include <linux/linkage.h>
#include <asm/unistd.h>

/*
 * Offsets into the rt_sigframe that the unwinder needs. These must agree with
 * setup_signal_stack_si() in arch/arm64/um/signal.c, which places
 *	sp -> struct rt_sigframe { siginfo info; struct ucontext uc; }
 * and puts the register state in uc.uc_mcontext.
 */
#define SIGFRAME_SIGINFO_SIZE	128			/* sizeof(struct siginfo) */
#define UCONTEXT_MCONTEXT_OFF	176			/* offsetof(ucontext, uc_mcontext) */
#define SIGCONTEXT_REGS_OFF	8			/* offsetof(sigcontext, regs) */
#define SIGFRAME_REGS_OFF	(SIGFRAME_SIGINFO_SIZE + \
				 UCONTEXT_MCONTEXT_OFF + SIGCONTEXT_REGS_OFF)

	.text

	.cfi_startproc
	.cfi_signal_frame
	.cfi_def_cfa	sp, 0
	.cfi_offset	x0,  SIGFRAME_REGS_OFF + 0 * 8
	.cfi_offset	x1,  SIGFRAME_REGS_OFF + 1 * 8
	.cfi_offset	x2,  SIGFRAME_REGS_OFF + 2 * 8
	.cfi_offset	x3,  SIGFRAME_REGS_OFF + 3 * 8
	.cfi_offset	x4,  SIGFRAME_REGS_OFF + 4 * 8
	.cfi_offset	x5,  SIGFRAME_REGS_OFF + 5 * 8
	.cfi_offset	x6,  SIGFRAME_REGS_OFF + 6 * 8
	.cfi_offset	x7,  SIGFRAME_REGS_OFF + 7 * 8
	.cfi_offset	x8,  SIGFRAME_REGS_OFF + 8 * 8
	.cfi_offset	x9,  SIGFRAME_REGS_OFF + 9 * 8
	.cfi_offset	x10, SIGFRAME_REGS_OFF + 10 * 8
	.cfi_offset	x11, SIGFRAME_REGS_OFF + 11 * 8
	.cfi_offset	x12, SIGFRAME_REGS_OFF + 12 * 8
	.cfi_offset	x13, SIGFRAME_REGS_OFF + 13 * 8
	.cfi_offset	x14, SIGFRAME_REGS_OFF + 14 * 8
	.cfi_offset	x15, SIGFRAME_REGS_OFF + 15 * 8
	.cfi_offset	x16, SIGFRAME_REGS_OFF + 16 * 8
	.cfi_offset	x17, SIGFRAME_REGS_OFF + 17 * 8
	.cfi_offset	x18, SIGFRAME_REGS_OFF + 18 * 8
	.cfi_offset	x19, SIGFRAME_REGS_OFF + 19 * 8
	.cfi_offset	x20, SIGFRAME_REGS_OFF + 20 * 8
	.cfi_offset	x21, SIGFRAME_REGS_OFF + 21 * 8
	.cfi_offset	x22, SIGFRAME_REGS_OFF + 22 * 8
	.cfi_offset	x23, SIGFRAME_REGS_OFF + 23 * 8
	.cfi_offset	x24, SIGFRAME_REGS_OFF + 24 * 8
	.cfi_offset	x25, SIGFRAME_REGS_OFF + 25 * 8
	.cfi_offset	x26, SIGFRAME_REGS_OFF + 26 * 8
	.cfi_offset	x27, SIGFRAME_REGS_OFF + 27 * 8
	.cfi_offset	x28, SIGFRAME_REGS_OFF + 28 * 8
	.cfi_offset	x29, SIGFRAME_REGS_OFF + 29 * 8
	.cfi_offset	x30, SIGFRAME_REGS_OFF + 30 * 8
	.cfi_offset	sp,  SIGFRAME_REGS_OFF + 31 * 8

SYM_FUNC_START(__kernel_rt_sigreturn)
	mov	x8, #__NR_rt_sigreturn
	svc	#0
SYM_FUNC_END(__kernel_rt_sigreturn)
	.cfi_endproc

	.section .note.GNU-stack,"",%progbits
