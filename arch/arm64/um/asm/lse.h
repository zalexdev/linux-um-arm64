/* SPDX-License-Identifier: GPL-2.0 */
#ifndef __ASM_LSE_H
#define __ASM_LSE_H

/*
 * arch/arm64/include/asm/lse.h selects between LSE and LL/SC atomics at runtime
 * with alternatives patching. UML has no alternatives patching: it is an
 * ordinary userspace process, there is no .altinstructions pass, and nothing
 * ever rewrites its text. Resolving the choice at compile time to LL/SC is
 * therefore not a simplification but the only correct option -- and LL/SC is
 * architecturally valid on every ARMv8 implementation, LSE or not.
 *
 * This is the same shape arm64 itself uses when CONFIG_ARM64_LSE_ATOMICS is
 * disabled, which keeps arm64's atomic.h and cmpxchg.h usable unmodified.
 */
#include <asm/atomic_ll_sc.h>

#define __lse_ll_sc_body(op, ...)		__ll_sc_##op(__VA_ARGS__)
#define ARM64_LSE_ATOMIC_INSN(llsc, lse)	llsc

#endif	/* __ASM_LSE_H */
