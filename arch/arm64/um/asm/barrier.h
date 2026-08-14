/* SPDX-License-Identifier: GPL-2.0 */
#ifndef _ASM_UM_ARM64_BARRIER_H_
#define _ASM_UM_ARM64_BARRIER_H_

/*
 * Inner-shareable DMBs. UML runs as an ordinary userspace process, so the
 * strongest domain it can name is the one its host threads share; "sy" would be
 * correct but needlessly expensive, and the load/store variants are what the
 * real arm64 port uses for rmb()/wmb().
 */
#define mb()	__asm__ __volatile__("dmb ish"   ::: "memory")
#define rmb()	__asm__ __volatile__("dmb ishld" ::: "memory")
#define wmb()	__asm__ __volatile__("dmb ishst" ::: "memory")

#define dma_rmb()	rmb()
#define dma_wmb()	wmb()

#include <asm-generic/barrier.h>

#endif
