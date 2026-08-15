/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * CPU capabilities for ARCH=um SUBARCH=arm64: there are none to discover.
 *
 * On hardware, cpus_have_final_cap() reports a capability bit established at
 * boot by reading ID registers at EL1. A UML guest runs at EL0 as an ordinary
 * process and cannot read them; arch/arm64/Kconfig, where every erratum symbol
 * is defined, is not sourced for ARCH=um, so none of those CONFIG symbols
 * exist here either.
 *
 * The reused arch/arm64/kernel/module-plts.c consults exactly one capability,
 * ARM64_WORKAROUND_843419 (a Cortex-A53 erratum affecting ADRP placement), and
 * consults it unconditionally rather than under an #ifdef. Answering "no" is
 * what the same code does on any part without the erratum, and is the only
 * answer this configuration can give honestly.
 *
 * Recorded as a limitation: a UML guest on an affected Cortex-A53 would not get
 * the PLT slack that hardware would. The guest executes at EL0 either way, so
 * this is the same exposure any userspace program on such a part has.
 */
#ifndef __UM_ARM64_CPUFEATURE_H
#define __UM_ARM64_CPUFEATURE_H

#include <linux/types.h>

#define ARM64_WORKAROUND_843419		0

static inline bool cpus_have_final_cap(int num)
{
	return false;
}

#endif /* __UM_ARM64_CPUFEATURE_H */
