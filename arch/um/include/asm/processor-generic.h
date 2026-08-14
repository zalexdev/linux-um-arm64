/* SPDX-License-Identifier: GPL-2.0 */
/* 
 * Copyright (C) 2000 - 2007 Jeff Dike (jdike@{addtoit,linux.intel}.com)
 */

#ifndef __UM_PROCESSOR_GENERIC_H
#define __UM_PROCESSOR_GENERIC_H

struct pt_regs;

struct task_struct;

#include <asm/ptrace.h>
#include <sysdep/archsetjmp.h>

#include <linux/prefetch.h>

#include <asm/cpuinfo.h>

struct mm_struct;

struct thread_struct {
	struct pt_regs *segv_regs;
	struct task_struct *prev_sched;
	struct arch_thread arch;
	jmp_buf switch_buf;
	struct {
		struct {
			int (*proc)(void *);
			void *arg;
		} thread;
	} request;

	void *segv_continue;

	/* Contains variable sized FP registers */
	struct pt_regs regs;
};

#define INIT_THREAD \
{ \
	.regs		   	= EMPTY_REGS,	\
	.prev_sched		= NULL, \
	.arch			= INIT_ARCH_THREAD, \
	.request		= { } \
}

/*
 * User space process size: 3GB (default).
 */
extern unsigned long task_size;

#define TASK_SIZE (task_size)

#undef STACK_TOP
#undef STACK_TOP_MAX

extern unsigned long stacksizelim;

#define STACK_ROOM	(stacksizelim)
#define STACK_TOP	(TASK_SIZE - 2 * PAGE_SIZE)
#define STACK_TOP_MAX	STACK_TOP

/* This decides where the kernel will search for a free chunk of vm
 * space during mmap's.
 */
#define TASK_UNMAPPED_BASE	(0x40000000)

extern void start_thread(struct pt_regs *regs, unsigned long entry, 
			 unsigned long stack);

struct cpuinfo_um {
	unsigned long loops_per_jiffy;
	int cache_alignment;
	/*
	 * Whatever the subarch needs to describe the host CPU to the guest.
	 * On x86 this is the mirrored CPUID feature bitmap; on arm64 it is the
	 * host's AT_HWCAP/AT_HWCAP2. Keeping it opaque here is what lets
	 * show_cpuinfo() below be architecture-neutral.
	 */
	struct arch_cpuinfo arch;
};

extern struct cpuinfo_um boot_cpu_data;

#define cache_line_size()	(boot_cpu_data.cache_alignment)

#define KSTK_REG(tsk, reg) get_thread_reg(reg, &tsk->thread.switch_buf)
extern unsigned long __get_wchan(struct task_struct *p);

#endif
