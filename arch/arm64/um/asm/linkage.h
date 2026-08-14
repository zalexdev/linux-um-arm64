/* SPDX-License-Identifier: GPL-2.0 */
#ifndef __UM_ARM64_LINKAGE_H
#define __UM_ARM64_LINKAGE_H

/*
 * arch/arm64/include/asm/linkage.h redefines SYM_FUNC_START and friends to emit
 * BTI landing pads, and pulls in <asm/assembler.h> to do it -- which drags in
 * alternatives, cpucaps and the rest of the EL1 assembler macros.
 *
 * A UML guest has no BTI (see arch/arm64/um/asm/mman.h), and the only assembly
 * UML/arm64 builds is its own setjmp and vDSO trampoline. The generic SYM_*
 * macros from <linux/linkage.h> are exactly right for those, so this header
 * intentionally adds nothing.
 */

#endif /* __UM_ARM64_LINKAGE_H */
