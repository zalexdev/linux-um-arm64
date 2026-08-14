/* SPDX-License-Identifier: GPL-2.0 */
#ifndef __UM_ARM64_WORD_AT_A_TIME_H
#define __UM_ARM64_WORD_AT_A_TIME_H

/*
 * arch/arm64/include/asm/word-at-a-time.h wraps its unaligned load helpers in
 * __mte_enable_tco_async()/__mte_disable_tco_async(), which flip PSTATE.TCO to
 * suppress MTE tag checks. PSTATE.TCO is EL0-writable only when the system has
 * MTE and the kernel has enabled it -- neither is true for a UML guest, which
 * has no MTE at all (see arch/arm64/um/asm/mman.h).
 *
 * The generic implementation is a plain byte-wise scan and is correct
 * everywhere; UML does not select DCACHE_WORD_ACCESS, so this is not on any hot
 * path.
 */
#include <asm-generic/word-at-a-time.h>

#endif
