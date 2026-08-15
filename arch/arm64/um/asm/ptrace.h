/* SPDX-License-Identifier: GPL-2.0 */
#ifndef __UM_ARM64_PTRACE_H
#define __UM_ARM64_PTRACE_H

/*
 * Regsets the UML core dump / ptrace glue knows about. arm64's real regset list
 * is much longer (SVE, SME, PAC keys, MTE, GCS...), but a UML guest has none of
 * those: it is a userspace process whose FP state is whatever the host gave the
 * stub. Only the two that exist for every aarch64 process are exposed.
 */
enum {
	REGSET_GENERAL,
	REGSET_FP,
	REGSET_SYSTEM_CALL,
};

#include <linux/compiler.h>
#include <uapi/asm/ptrace.h>
#include <asm/ptrace-generic.h>

#define user_mode(r) UPT_IS_USER(&(r)->regs)

#define PT_REGS_X(r, n)	UPT_X(&(r)->regs, n)
#define PT_REGS_SP_REG(r) UPT_SP(&(r)->regs)
#define PT_REGS_PSTATE(r) UPT_PSTATE(&(r)->regs)

#define PT_REGS_X0(r)	PT_REGS_X(r, HOST_X0)
#define PT_REGS_X1(r)	PT_REGS_X(r, HOST_X1)
#define PT_REGS_X2(r)	PT_REGS_X(r, HOST_X2)
#define PT_REGS_X3(r)	PT_REGS_X(r, HOST_X3)
#define PT_REGS_X4(r)	PT_REGS_X(r, HOST_X4)
#define PT_REGS_X5(r)	PT_REGS_X(r, HOST_X5)
#define PT_REGS_X6(r)	PT_REGS_X(r, HOST_X6)
#define PT_REGS_X7(r)	PT_REGS_X(r, HOST_X7)
#define PT_REGS_X8(r)	PT_REGS_X(r, HOST_X8)
#define PT_REGS_X29(r)	PT_REGS_X(r, HOST_X29)
#define PT_REGS_X30(r)	PT_REGS_X(r, HOST_X30)

/* Frame pointer, for the generic KSTK_EBP/backtrace glue. */
#define PT_REGS_BP(r)	PT_REGS_X29(r)

/*
 * The syscall return value is x0.
 *
 * PT_REGS_ORIG_SYSCALL is NOT x0. On x86 the two alias because RAX carries both
 * the syscall number in and the result out, so arch/um/kernel/signal.c can write
 * "PT_REGS_ORIG_SYSCALL(regs) = __NR_restart_syscall" and mean it. On arm64 the
 * number and the result live in different registers, so aliasing them would make
 * that assignment clobber the return value instead of redirecting the syscall.
 * It maps to the synthesised HOST_SYSCALL_NR slot, which is also what the
 * ptrace and seccomp resume paths write back to the host.
 */
#define PT_REGS_SYSCALL_RET(r)	PT_REGS_X0(r)
#define PT_REGS_ORIG_SYSCALL(r)	PT_REGS_X(r, HOST_SYSCALL_NR)

#define PT_REGS_SET_SYSCALL_RETURN(r, res) (PT_REGS_X0(r) = (res))

/*
 * Bits of PSTATE that a debugger (or a sigreturn frame) may set, mirroring
 * valid_native_regs() in arch/arm64/kernel/ptrace.c. Anything outside this
 * either names an exception level the guest cannot be at or masks interrupts it
 * does not own; the host would sanitise it on PTRACE_SETREGSET regardless, so
 * filtering here keeps the guest's own view consistent with what will actually
 * be installed.
 */
#define UM_PSTATE_WRITABLE						\
	(PSR_N_BIT | PSR_Z_BIT | PSR_C_BIT | PSR_V_BIT |		\
	 PSR_SSBS_BIT | PSR_DIT_BIT | PSR_TCO_BIT)

#define PT_FIX_EXEC_STACK(sp) do ; while (0)

#define profile_pc(regs) PT_REGS_IP(regs)

static inline long regs_return_value(struct pt_regs *regs)
{
	return PT_REGS_X0(regs);
}

#define user_stack_pointer(regs) PT_REGS_SP(regs)

/*
 * arm64 has no LDT and no set_thread_area(2); TLS is TPIDR_EL0, handled through
 * arch_set_tls(). These exist only because arch/um/kernel/ptrace.c references
 * them unconditionally.
 */
struct user_desc;

#include <asm/errno.h>

static inline int ptrace_get_thread_area(struct task_struct *child, int idx,
					 struct user_desc __user *user_desc)
{
	return -ENOSYS;
}

static inline int ptrace_set_thread_area(struct task_struct *child, int idx,
					 struct user_desc __user *user_desc)
{
	return -ENOSYS;
}

extern void arch_switch_to(struct task_struct *to);

#endif /* __UM_ARM64_PTRACE_H */
