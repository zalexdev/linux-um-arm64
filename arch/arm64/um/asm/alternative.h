/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * Alternatives for ARCH=um SUBARCH=arm64: there are none.
 *
 * arch/arm64/kernel/module.c is reused here for module loading, and it calls
 * apply_alternatives_module() on any `.altinstructions` section a module
 * carries. On hardware that patches instruction sequences according to CPU
 * capabilities discovered at boot.
 *
 * A UML guest has no such capabilities to discover: it is an ordinary
 * userspace process, arch/arm64/Kconfig -- which is where every erratum and
 * feature symbol lives -- is not sourced for ARCH=um, and no code built for
 * this configuration emits ALTERNATIVE sequences. So a UML module has no
 * `.altinstructions` section, module_finalize() never finds one, and this
 * function is never called.
 *
 * It exists so the shared module.c compiles. The alternative would be to pull
 * in arch/arm64/include/asm/alternative.h, which reaches asm/cpucaps.h and the
 * generated asm/cpucap-defs.h, i.e. the whole cpufeature apparatus, to reach a
 * call that cannot happen.
 */
#ifndef __UM_ARM64_ALTERNATIVE_H
#define __UM_ARM64_ALTERNATIVE_H

#include <linux/types.h>

static inline int apply_alternatives_module(void *start, size_t length)
{
	return 0;
}

#endif /* __UM_ARM64_ALTERNATIVE_H */
