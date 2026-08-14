// SPDX-License-Identifier: GPL-2.0
/*
 * Guest TLS for ARCH=um SUBARCH=arm64.
 *
 * arm64 thread-local storage is the TPIDR_EL0 system register, which EL0 reads
 * and writes itself -- there is no set_thread_area(2) and no descriptor table.
 * That means UML never has to intercept a TLS *change*; it only has to make
 * sure the right value is in the register when it resumes a given guest thread,
 * since several guest threads share one stub process.
 */
#include <linux/sched.h>
#include <asm/ptrace.h>

void clear_flushed_tls(struct task_struct *task)
{
}

/*
 * Make sure @to's TPIDR_EL0 is what will be in the register when it runs.
 *
 * Guest threads sharing an mm share one stub process, so the TLS register has
 * to be reinstated on every switch between them -- it is not part of the GPR
 * set that gets restored wholesale. Recording it in the register shadow slot is
 * enough: the ptrace path writes NT_ARM_TLS from there in put_host_regs(), and
 * the seccomp path hands it to the stub in set_stub_state().
 */
void arch_switch_tls(struct task_struct *to)
{
	to->thread.regs.regs.gp[HOST_TLS] = to->thread.arch.tp_value;
}

int arch_set_tls(struct task_struct *t, unsigned long tls)
{
	/*
	 * CLONE_SETTLS: remember the value so the next switch to this task
	 * installs it. x86-64 can stash this straight into the ptrace register
	 * set because FS_BASE is part of user_regs_struct; TPIDR_EL0 is a
	 * separate regset (NT_ARM_TLS), so it needs its own home in
	 * arch_thread.
	 */
	t->thread.arch.tp_value = tls;
	t->thread.regs.regs.gp[HOST_TLS] = tls;

	return 0;
}
