/* SPDX-License-Identifier: GPL-2.0 */
/* 
 * Copyright (C) 2002 - 2007 Jeff Dike (jdike@{addtoit,linux.intel}.com)
 */

#ifndef __ARCH_UM_MMU_H
#define __ARCH_UM_MMU_H

#include "linux/types.h"
#include <linux/mutex.h>
#include <linux/spinlock.h>
#include <mm_id.h>

typedef struct mm_context {
	struct mm_id id;
	struct mutex turnstile;

	struct list_head list;

	/* Address range in need of a TLB sync */
	spinlock_t sync_tlb_lock;
	unsigned long sync_tlb_range_from;
	unsigned long sync_tlb_range_to;

	/*
	 * Anonymous fault-around state (arch/um/kernel/trap.c). A fault costs a
	 * full stub handoff (~51us measured on-device in seccomp mode), so
	 * sequential fault streams are detected and additional pages are
	 * prefaulted into the same handoff. prefault_next is the page a
	 * sequential stream would fault next (end of the last window);
	 * prefault_level indexes the ramp. Purely a heuristic: fields are
	 * accessed racily (READ_ONCE/WRITE_ONCE only) under mmap_read_lock,
	 * a lost update just mispredicts one window.
	 */
	unsigned long prefault_next;
	unsigned int prefault_level;
} mm_context_t;

#define INIT_MM_CONTEXT(mm)						\
	.context = {							\
		.turnstile = __MUTEX_INITIALIZER(mm.context.turnstile),	\
		.sync_tlb_lock = __SPIN_LOCK_INITIALIZER(mm.context.sync_tlb_lock), \
	}

#endif
