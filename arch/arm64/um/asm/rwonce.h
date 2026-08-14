/* SPDX-License-Identifier: GPL-2.0 */
#ifndef __UM_ARM64_RWONCE_H
#define __UM_ARM64_RWONCE_H

/*
 * arch/arm64/include/asm/rwonce.h exists to give LTO builds a load-acquire
 * READ_ONCE() via alternatives patching, which drags in asm/cpucaps.h,
 * asm/alternative-macros.h and the whole EL1 cpufeature machinery. A UML guest
 * is an ordinary userspace process with no alternatives patching and no system
 * registers, so the generic definitions are both correct and the only ones that
 * can compile here.
 */
#include <asm-generic/rwonce.h>

#endif
