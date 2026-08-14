// SPDX-License-Identifier: GPL-2.0
#include <linux/kernel.h>
#include <linux/module.h>
#include <linux/pid.h>
#include <linux/sched.h>
#include <linux/sched/debug.h>
#include <linux/utsname.h>
#include <asm/current.h>
#include <asm/ptrace.h>

void show_regs(struct pt_regs *regs)
{
	int i;

	printk("\n");
	print_modules();
	printk(KERN_INFO "Pid: %d, comm: %.20s %s %s\n", task_pid_nr(current),
	       current->comm, print_tainted(), init_utsname()->release);

	printk(KERN_INFO "pc : %pS\n", (void *)PT_REGS_IP(regs));
	printk(KERN_INFO "lr : %pS\n", (void *)PT_REGS_X30(regs));
	printk(KERN_INFO "sp : %016lx  pstate : %08lx\n",
	       PT_REGS_SP(regs), PT_REGS_PSTATE(regs));

	/*
	 * Three registers per line, highest first, matching the layout
	 * arch/arm64/kernel/traps.c uses so that arm64 developers can read a UML
	 * oops without re-learning it.
	 */
	for (i = 29; i >= 0; i -= 3) {
		if (i >= 2)
			printk(KERN_INFO "x%-2d: %016lx x%-2d: %016lx x%-2d: %016lx\n",
			       i, PT_REGS_X(regs, HOST_X0 + i),
			       i - 1, PT_REGS_X(regs, HOST_X0 + i - 1),
			       i - 2, PT_REGS_X(regs, HOST_X0 + i - 2));
		else if (i == 1)
			printk(KERN_INFO "x1 : %016lx x0 : %016lx\n",
			       PT_REGS_X(regs, HOST_X0 + 1),
			       PT_REGS_X(regs, HOST_X0));
		else
			printk(KERN_INFO "x0 : %016lx\n", PT_REGS_X(regs, HOST_X0));
	}
}
