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

/*
 * Install this task's TLS before it runs.
 *
 * On x86-64 this function is empty, because FS_BASE lives inside the ptrace
 * register set and is therefore restored along with everything else. arm64's
 * TPIDR_EL0 is a separate regset, so it has to be pushed across explicitly: the
 * value is copied into the stub's arch data and flagged, and the stub reinstates
 * it with a single "msr tpidr_el0" on resume (see stub_seccomp_restore_state()).
 */
void arch_switch_to(struct task_struct *to)
{
	arch_switch_tls(to);
}
