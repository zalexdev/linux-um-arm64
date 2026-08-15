/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * Kernel-mode SIMD for ARCH=um SUBARCH=arm64: not available.
 *
 * On hardware, may_use_simd() reports whether the kernel may currently use the
 * FPSIMD/NEON registers, which is safe at EL1 only because arch/arm64 saves and
 * restores the interrupted context's FP state around it (kernel_neon_begin/end).
 *
 * A UML guest kernel has no such mechanism: it runs inside a userspace process
 * and shares one FPSIMD register file with the guest userspace it is servicing.
 * Using NEON in guest kernel code would silently corrupt the registers of
 * whatever guest thread happened to be running -- which is precisely the class
 * of bug the FP/SIMD torture gate exists to catch.
 *
 * Reporting "no" makes every crypto driver and library that offers a SIMD
 * implementation fall back to its generic C one, which is the correct and only
 * safe answer here.
 *
 * This also keeps <asm/simd.h> from resolving to arch/arm64's, which reaches
 * asm/neon.h, asm/fpsimd.h and finally the generated asm/sysreg-defs.h that is
 * not built for ARCH=um.
 */
#ifndef __UM_ARM64_SIMD_H
#define __UM_ARM64_SIMD_H

#include <asm-generic/simd.h>

#endif /* __UM_ARM64_SIMD_H */
