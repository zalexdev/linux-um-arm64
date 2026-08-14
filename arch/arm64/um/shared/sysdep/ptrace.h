/* SPDX-License-Identifier: GPL-2.0 */
#ifndef __SYSDEP_ARM64_PTRACE_H
#define __SYSDEP_ARM64_PTRACE_H

#include <generated/user_constants.h>
#include <sysdep/faultinfo.h>

/*
 * gp[] mirrors struct user_regs_struct (== struct user_pt_regs) for the first
 * UM_FRAME_SIZE bytes, followed by two slots UML synthesises itself. Only the
 * first UM_FRAME_SIZE bytes are ever handed to the host; see user-offsets.c.
 */
#define MAX_REG_OFFSET (UM_FRAME_SIZE)
#define MAX_REG_NR (UM_MAX_REG_NR)

#define REGS_X(r, n)	((r)[(n)])
#define REGS_SP(r)	((r)[HOST_SP])
#define REGS_IP(r)	((r)[HOST_PC])
#define REGS_PSTATE(r)	((r)[HOST_PSTATE])
#define REGS_ORIG_X0(r)	((r)[HOST_ORIG_X0])
#define REGS_SYSCALL_NR(r) ((r)[HOST_SYSCALL_NR])
#define REGS_TLS(r)	((r)[HOST_TLS])

#define UPT_X(r, n)	REGS_X((r)->gp, n)
#define UPT_SP(r)	REGS_SP((r)->gp)
#define UPT_IP(r)	REGS_IP((r)->gp)
#define UPT_PSTATE(r)	REGS_PSTATE((r)->gp)
#define UPT_ORIG_X0(r)	REGS_ORIG_X0((r)->gp)
#define UPT_TLS(r)	REGS_TLS((r)->gp)

/*
 * AAPCS64 syscall ABI: arguments in x0..x5, number in x8, result in x0.
 *
 * Argument 1 is read from the saved ORIG_X0 slot, not from the live x0.
 *
 * The syscall return value and the first syscall argument are the same register
 * on arm64. handle_syscall() in arch/um/kernel/skas/syscall.c seeds the return
 * value with -ENOSYS *before* it reads the arguments -- which is harmless on
 * x86, where the return lands in RAX and argument 1 comes from RDI, but on arm64
 * it would destroy argument 1 in every single syscall. (The symptom is that
 * every guest syscall sees -38 as its first argument, so the very first
 * open/write from init fails and userspace dies before printing anything.)
 *
 * get_host_regs() and get_regs_from_mc() both capture x0 into HOST_ORIG_X0 at
 * the point the guest trapped, which is the value the ABI actually passed.
 */
#define UPT_SYSCALL_ARG1(r) UPT_ORIG_X0(r)
#define UPT_SYSCALL_ARG2(r) UPT_X(r, HOST_X1)
#define UPT_SYSCALL_ARG3(r) UPT_X(r, HOST_X2)
#define UPT_SYSCALL_ARG4(r) UPT_X(r, HOST_X3)
#define UPT_SYSCALL_ARG5(r) UPT_X(r, HOST_X4)
#define UPT_SYSCALL_ARG6(r) UPT_X(r, HOST_X5)

/*
 * A syscall is restarted by rewinding over the "svc #0" that issued it. arm64
 * instructions are fixed 4-byte width, so this is exact; x86 subtracts 2 for
 * its variable-length "syscall".
 *
 * Two registers have to be put back, and both differ from x86:
 *
 *   x0  by the time we decide to restart, it holds -ERESTARTSYS rather than the
 *       first argument. x86 does not need this because there the syscall number
 *       and the return value share one register, so restoring the number
 *       restores the argument too.
 *
 *   x8  because the guest is about to *re-execute* the svc, and it is x8 that
 *       selects the syscall at that point -- not NT_ARM_SYSTEM_CALL, which only
 *       redirects the syscall already in flight. Without this,
 *       "PT_REGS_ORIG_SYSCALL(regs) = __NR_restart_syscall" in
 *       arch/um/kernel/signal.c silently has no effect: the guest re-runs the
 *       original syscall instead of restart_syscall, so a signal-interrupted
 *       nanosleep restarts its full duration rather than its remainder. On x86
 *       ORIG_SYSCALL is a register the kernel really does reload, so the same
 *       code works there.
 *
 * The two call sites in arch/um/kernel/signal.c assign ORIG_SYSCALL before and
 * after this macro respectively; taking the number from the slot rather than
 * from a parameter is what makes both orderings come out right.
 */
#define UPT_RESTART_SYSCALL(r)						\
	do {								\
		long __nr = (long)REGS_SYSCALL_NR((r)->gp);		\
									\
		UPT_IP(r) -= 4;						\
		UPT_X(r, HOST_X0) = UPT_ORIG_X0(r);			\
		/*							\
		 * Only reload x8 when the slot really holds a syscall	\
		 * number. It comes from NT_ARM_SYSTEM_CALL, which the	\
		 * host declines to supply outside a syscall stop,	\
		 * leaving -1 -- and writing that into x8 would make the	\
		 * guest re-execute the svc with a number nobody asked	\
		 * for.							\
		 */							\
		if (__nr >= 0)						\
			UPT_X(r, HOST_X8) = (unsigned long)__nr;	\
	} while (0)

extern unsigned long host_fp_size;

struct uml_pt_regs {
	unsigned long gp[MAX_REG_NR];
	struct faultinfo faultinfo;
	long syscall;
	int is_user;

	/* Dynamically sized FP registers (FPSIMD, or FPSIMD+SVE) */
	unsigned long fp[];
};

#define EMPTY_UML_PT_REGS { }

#define UPT_SYSCALL_NR(r) ((r)->syscall)
#define UPT_FAULTINFO(r) (&(r)->faultinfo)
#define UPT_IS_USER(r) ((r)->is_user)

extern int arch_init_registers(int pid);

#endif /* __SYSDEP_ARM64_PTRACE_H */
