// SPDX-License-Identifier: GPL-2.0
/*
 * Generates include/generated/user_constants.h for ARCH=um SUBARCH=arm64.
 *
 * This file is compiled with USER_CFLAGS against the *host* headers, so
 * everything it sees describes the aarch64 process UML runs as.
 */
#include <stdio.h>
#include <stddef.h>
#include <signal.h>
#include <poll.h>
#include <sys/mman.h>
#include <sys/user.h>
#include <asm/ptrace.h>
#include <asm/types.h>
#include <linux/kbuild.h>

#define DEFINE_LONGS(sym, val)	\
	COMMENT(#val " / sizeof(unsigned long)");	\
	DEFINE(sym, val / sizeof(unsigned long))

/* workaround for a warning with -Wmissing-prototypes */
void foo(void);

void foo(void)
{
	/*
	 * struct user_regs_struct on arm64 is struct user_pt_regs:
	 *	__u64 regs[31]; __u64 sp; __u64 pc; __u64 pstate;
	 * so the general-purpose registers are simply indices 0..30 and the
	 * three specials follow. Unlike x86 there are no segment registers and
	 * no flags word -- pstate serves the role of eflags but is not writable
	 * in the same freewheeling way.
	 */
	DEFINE_LONGS(HOST_X0,  offsetof(struct user_regs_struct, regs[0]));
	DEFINE_LONGS(HOST_X1,  offsetof(struct user_regs_struct, regs[1]));
	DEFINE_LONGS(HOST_X2,  offsetof(struct user_regs_struct, regs[2]));
	DEFINE_LONGS(HOST_X3,  offsetof(struct user_regs_struct, regs[3]));
	DEFINE_LONGS(HOST_X4,  offsetof(struct user_regs_struct, regs[4]));
	DEFINE_LONGS(HOST_X5,  offsetof(struct user_regs_struct, regs[5]));
	DEFINE_LONGS(HOST_X6,  offsetof(struct user_regs_struct, regs[6]));
	DEFINE_LONGS(HOST_X7,  offsetof(struct user_regs_struct, regs[7]));
	DEFINE_LONGS(HOST_X8,  offsetof(struct user_regs_struct, regs[8]));
	DEFINE_LONGS(HOST_X29, offsetof(struct user_regs_struct, regs[29]));
	DEFINE_LONGS(HOST_X30, offsetof(struct user_regs_struct, regs[30]));

	DEFINE_LONGS(HOST_SP,     offsetof(struct user_regs_struct, sp));
	DEFINE_LONGS(HOST_PC,     offsetof(struct user_regs_struct, pc));
	DEFINE_LONGS(HOST_PSTATE, offsetof(struct user_regs_struct, pstate));

	/*
	 * UM_FRAME_SIZE is the size of the register block that is actually
	 * exchanged with the host via PTRACE_GETREGSET/NT_PRSTATUS. It is the
	 * bound for peek_user/poke_user and for every ptrace transfer.
	 */
	DEFINE(UM_FRAME_SIZE, sizeof(struct user_regs_struct));

	/*
	 * Two slots past the end of the host regset, synthesised by UML:
	 *
	 *   HOST_ORIG_X0     x0 as it was on entry to a syscall. arm64's kernel
	 *                    keeps orig_x0 in its struct pt_regs but does not
	 *                    expose it through NT_PRSTATUS, and by the time a
	 *                    syscall needs restarting x0 holds -ERESTARTSYS.
	 *   HOST_SYSCALL_NR  the syscall number, read from NT_ARM_SYSTEM_CALL
	 *                    (ptrace mode) or from x8 (seccomp mode). Writing x8
	 *                    does not change which syscall runs, so this cannot
	 *                    simply alias HOST_X8.
	 *   HOST_TLS         guest TPIDR_EL0. x86-64 keeps the equivalent
	 *                    (FS_BASE) inside user_regs_struct and so gets it
	 *                    saved and restored for free; arm64 exposes TPIDR_EL0
	 *                    only through the separate NT_ARM_TLS regset, so UML
	 *                    has to carry it alongside the GPRs itself.
	 *
	 * x86 gets both of these free inside user_regs_struct as orig_ax; arm64
	 * does not, which is why they are appended here rather than aliased.
	 */
	DEFINE(HOST_ORIG_X0,    sizeof(struct user_regs_struct) / sizeof(unsigned long));
	DEFINE(HOST_SYSCALL_NR, sizeof(struct user_regs_struct) / sizeof(unsigned long) + 1);
	DEFINE(HOST_TLS,        sizeof(struct user_regs_struct) / sizeof(unsigned long) + 2);
	DEFINE(UM_MAX_REG_NR,   sizeof(struct user_regs_struct) / sizeof(unsigned long) + 3);

	DEFINE(UM_POLLIN, POLLIN);
	DEFINE(UM_POLLPRI, POLLPRI);
	DEFINE(UM_POLLOUT, POLLOUT);

	DEFINE(UM_PROT_READ, PROT_READ);
	DEFINE(UM_PROT_WRITE, PROT_WRITE);
	DEFINE(UM_PROT_EXEC, PROT_EXEC);
}
