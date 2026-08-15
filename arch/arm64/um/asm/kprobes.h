/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * ARCH=um SUBARCH=arm64 has no kprobes.
 *
 * arch/arm64/lib/insn.c is reused here for instruction encoding and tags some
 * of its functions __kprobes, which is a section annotation rather than a
 * dependency on the kprobes machinery. The generic header supplies it.
 *
 * Without this, <asm/kprobes.h> resolves to arch/arm64's, which reaches
 * asm/cpucaps.h and the generated asm/cpucap-defs.h -- the whole cpufeature
 * apparatus -- to obtain a macro that expands to nothing here.
 */
#ifndef __UM_ARM64_KPROBES_H
#define __UM_ARM64_KPROBES_H

#include <asm-generic/kprobes.h>

#endif /* __UM_ARM64_KPROBES_H */
