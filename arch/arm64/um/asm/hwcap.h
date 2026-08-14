/* SPDX-License-Identifier: GPL-2.0 */
#ifndef __UM_ARM64_HWCAP_H
#define __UM_ARM64_HWCAP_H

/*
 * arch/arm64/include/uapi/asm/ptrace.h includes <asm/hwcap.h>, which in a real
 * arm64 build lands on the kernel-side header: KERNEL_HWCAP_* ordinals, the
 * cpu_have_named_feature() helper and, through <asm/cpufeature.h>, the whole
 * alternatives and cpucaps machinery.
 *
 * A UML guest has no cpucaps -- it never probes ID registers, and its notion of
 * "what this CPU can do" is the AT_HWCAP/AT_HWCAP2 the host handed the UML
 * process (see arch/arm64/um/cpuinfo.c). Only the uapi HWCAP_* bit definitions
 * are meaningful here, and those are what guest userspace sees anyway.
 */
#include <uapi/asm/hwcap.h>

#endif /* __UM_ARM64_HWCAP_H */
