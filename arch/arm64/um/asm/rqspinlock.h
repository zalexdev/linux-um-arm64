/* SPDX-License-Identifier: GPL-2.0 */
#ifndef _ASM_UM_ARM64_RQSPINLOCK_H_
#define _ASM_UM_ARM64_RQSPINLOCK_H_

/*
 * Take the generic resilient queued spinlock, not arm64's.
 *
 * arch/arm64/include/asm/rqspinlock.h builds res_smp_cond_load_acquire() on
 * smp_cond_load_acquire_timewait(), which waits in WFE and therefore has to ask
 * arch_timer_evtstrm_available() whether the architected timer's event stream
 * is running to wake it. Neither exists here: a UML guest is an ordinary
 * userspace process with no architected timer of its own, and the symbol is not
 * merely missing but meaningless.
 *
 * Without this the build breaks the moment SMP and BPF are both on, which is
 * the ordinary configuration for running containers:
 *
 *	arch/arm64/include/asm/rqspinlock.h:72:14: error: call to undeclared
 *	function 'arch_timer_evtstrm_available'
 *	  int __wfe = arch_timer_evtstrm_available();
 *
 * Defining nothing is the fix rather than a workaround: kernel/bpf/rqspinlock.c
 * guards the macro with #ifndef and falls back to plain smp_cond_load_acquire,
 * which is correct on any architecture and is what a guest wants anyway --
 * spinning in the guest is spinning in a host thread, and the host scheduler,
 * not an event stream, is what eventually runs the lock holder.
 */
#include <asm-generic/rqspinlock.h>

#endif
