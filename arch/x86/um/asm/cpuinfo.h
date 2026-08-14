/* SPDX-License-Identifier: GPL-2.0 */
#ifndef __UM_X86_CPUINFO_H
#define __UM_X86_CPUINFO_H

#include <asm/cpufeatures.h>

/*
 * Subarch half of struct cpuinfo_um. x86 mirrors the host's CPUID feature bits
 * so that /proc/cpuinfo inside the guest lists the same flags the host reports.
 */
struct arch_cpuinfo {
	union {
		__u32		x86_capability[NCAPINTS + NBUGINTS];
		unsigned long	x86_capability_alignment;
	};
};

#endif /* __UM_X86_CPUINFO_H */
