/* SPDX-License-Identifier: GPL-2.0 */
#ifndef __UM_ARM64_CPUINFO_H
#define __UM_ARM64_CPUINFO_H

#include <linux/types.h>

/*
 * Subarch half of struct cpuinfo_um.
 *
 * arm64 has no CPUID feature bitmap to mirror. What a guest can actually rely
 * on is exactly what the host passed in the ELF auxiliary vector: AT_HWCAP and
 * AT_HWCAP2, which is also what /proc/cpuinfo's "Features" line is generated
 * from on real arm64. Storing those two words -- rather than inventing a
 * UML-specific feature namespace -- keeps the guest's view honest: it advertises
 * a feature if and only if the host told the UML process it had one.
 */
struct arch_cpuinfo {
	unsigned long hwcap;
	unsigned long hwcap2;
};

#endif /* __UM_ARM64_CPUINFO_H */
