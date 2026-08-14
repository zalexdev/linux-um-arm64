/* SPDX-License-Identifier: GPL-2.0 */
#ifndef __UM_ARM64_PROCESSOR_H
#define __UM_ARM64_PROCESSOR_H

#include <linux/time-internal.h>

/* include faultinfo structure */
#include <sysdep/faultinfo.h>

struct arch_thread {
	/*
	 * Guest TLS (TPIDR_EL0). arm64 userspace owns this register outright,
	 * so UML only has to remember it across a switch between guest threads
	 * sharing one stub process.
	 */
	unsigned long tp_value;
	struct faultinfo faultinfo;
};

#define INIT_ARCH_THREAD {			\
	.tp_value	= 0,			\
	.faultinfo	= { 0, 0, 0 }		\
}

#define STACKSLOTS_PER_LINE 4

static inline void arch_flush_thread(struct arch_thread *thread)
{
	thread->tp_value = 0;
}

static inline void arch_copy_thread(struct arch_thread *from,
				    struct arch_thread *to)
{
	to->tp_value = from->tp_value;
}

#define current_sp()						\
	({ void *sp; __asm__("mov %0, sp" : "=r" (sp)); sp; })
#define current_bp()						\
	({ unsigned long bp; __asm__("mov %0, x29" : "=r" (bp)); bp; })

#define KSTK_EIP(tsk) KSTK_REG(tsk, HOST_PC)
#define KSTK_ESP(tsk) KSTK_REG(tsk, HOST_SP)
#define KSTK_EBP(tsk) KSTK_REG(tsk, HOST_X29)

#define ARCH_IS_STACKGROW(address) \
	(address + 65536 + 32 * sizeof(unsigned long) >= UPT_SP(&current->thread.regs.regs))

/*
 * x86 emits PAUSE here. The aarch64 equivalent is YIELD, a hint that costs
 * nothing on cores that ignore it and lets an SMT core give up its slot.
 */
static __always_inline void native_pause(void)
{
	__asm__ __volatile__("yield" ::: "memory");
}

static __always_inline void cpu_relax(void)
{
	if (time_travel_mode == TT_MODE_INFCPU ||
	    time_travel_mode == TT_MODE_EXTERNAL)
		time_travel_ndelay(1);
	else
		native_pause();
}

#define task_pt_regs(t) (&(t)->thread.regs)

#include <asm/processor-generic.h>

#endif /* __UM_ARM64_PROCESSOR_H */
