// SPDX-License-Identifier: GPL-2.0
/*
 * Architecture-specific syscalls for UML/arm64.
 */
#include <linux/sched.h>
#include <linux/sched/mm.h>
#include <linux/syscalls.h>
#include <linux/uaccess.h>
#include <asm/ptrace.h>
#include <registers.h>
#include <os.h>

/*
 * arm64's sys_mmap takes a byte offset, not a page offset (that is what
 * distinguishes it from mmap2 on 32-bit architectures). The real
 * implementation lives in arch/arm64/kernel/sys.c, which is not built for
 * ARCH=um, so it is repeated here -- as x86-64 UML does for the same reason.
 */
SYSCALL_DEFINE6(mmap, unsigned long, addr, unsigned long, len,
		unsigned long, prot, unsigned long, flags,
		unsigned long, fd, unsigned long, off)
{
	if (offset_in_page(off) != 0)
		return -EINVAL;

	return ksys_mmap_pgoff(addr, len, prot, flags, fd, off >> PAGE_SHIFT);
}

void arch_switch_to(struct task_struct *to)
{
	/*
	 * Nothing to do, and it is important that nothing is done.
	 *
	 * arm64 userspace owns TPIDR_EL0 outright: the guest sets its own thread
	 * pointer with "msr tpidr_el0, xN" and never asks the kernel. UML
	 * captures whatever the guest put there in get_host_regs() (from
	 * NT_ARM_TLS) or get_regs_from_mc(), stores it in the task's own
	 * HOST_TLS register slot, and writes it back in put_host_regs(). Since
	 * that slot is part of the per-task register file, switching tasks
	 * already switches the TLS.
	 *
	 * Reinstating thread.arch.tp_value here instead would be actively wrong:
	 * that field is only ever set by CLONE_SETTLS, so for a thread that set
	 * its own TLS -- which is every normal thread -- it is zero, and copying
	 * it over the live value wipes the guest's thread pointer on the first
	 * context switch. musl notices immediately: __dls3 compares
	 * __pthread_self() against TPIDR_EL0 and traps.
	 */
}
