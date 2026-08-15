// SPDX-License-Identifier: GPL-2.0
/*
 * Copyright (C) 2002 - 2007 Jeff Dike (jdike@{addtoit,linux.intel}.com)
 */

#include <linux/kernel.h>
#include <linux/ptrace.h>
#include <linux/seccomp.h>
#include <kern_util.h>
#include <sysdep/ptrace.h>
#include <sysdep/ptrace_user.h>
#include <linux/time-internal.h>
#include <asm/syscall.h>
#include <asm/unistd.h>
#include <asm/delay.h>

/*
 * Seed the return register with -ENOSYS before the syscall-entry stop.
 *
 * Unconditional unless a subarch says otherwise, which preserves the x86
 * behaviour: there the return register is not an argument register and native
 * x86 genuinely leaves -ENOSYS in it at an entry stop. See the arm64 override
 * in <sysdep/ptrace.h> for why that is wrong when the two alias.
 */
#ifndef UM_SEED_ENOSYS_BEFORE_TRACE
#define UM_SEED_ENOSYS_BEFORE_TRACE(r)	1
#endif

void handle_syscall(struct uml_pt_regs *r)
{
	struct pt_regs *regs = container_of(r, struct pt_regs, regs);
	int syscall;

	/* Initialize the syscall number and default return value. */
	UPT_SYSCALL_NR(r) = PT_SYSCALL_NR(r->gp);
	if (UM_SEED_ENOSYS_BEFORE_TRACE(r))
		PT_REGS_SET_SYSCALL_RETURN(regs, -ENOSYS);


	if (syscall_trace_enter(regs)) {
		/*
		 * The tracer cancelled the call. Leave the return register
		 * alone: a tracer that skips a syscall is expected to have set
		 * the value it wants userspace to see, and on architectures
		 * where that register is also an argument register, clobbering
		 * it here would destroy an argument the tracer may have just
		 * written.
		 */
		goto out;
	}

	/* Do the seccomp check after ptrace; failures should be fast. */
	if (secure_computing() == -1)
		goto out;

	syscall = UPT_SYSCALL_NR(r);

	/*
	 * If no time passes, then sched_yield may not actually yield, causing
	 * broken spinlock implementations in userspace (ASAN) to hang for long
	 * periods of time.
	 */
	if ((time_travel_mode == TT_MODE_INFCPU ||
	     time_travel_mode == TT_MODE_EXTERNAL) &&
	    syscall == __NR_sched_yield)
		tt_extra_sched_jiffies += 1;

	if (syscall >= 0 && syscall < __NR_syscalls) {
		unsigned long ret;

		ret = (*sys_call_table[syscall])(UPT_SYSCALL_ARG1(&regs->regs),
						 UPT_SYSCALL_ARG2(&regs->regs),
						 UPT_SYSCALL_ARG3(&regs->regs),
						 UPT_SYSCALL_ARG4(&regs->regs),
						 UPT_SYSCALL_ARG5(&regs->regs),
						 UPT_SYSCALL_ARG6(&regs->regs));

		PT_REGS_SET_SYSCALL_RETURN(regs, ret);

		/*
		 * An error value here can be some form of -ERESTARTSYS
		 * and then we'd just loop. Make any error syscalls take
		 * some time, so that it won't just loop if something is
		 * not ready, and hopefully other things will make some
		 * progress.
		 */
		if (IS_ERR_VALUE(ret) &&
		    (time_travel_mode == TT_MODE_INFCPU ||
		     time_travel_mode == TT_MODE_EXTERNAL)) {
			um_udelay(1);
			schedule();
		}
	}

out:
	syscall_trace_leave(regs);
}
