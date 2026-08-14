/* SPDX-License-Identifier: GPL-2.0 */
#ifndef __SYSDEP_ARM64_KERNEL_OFFSETS_H
#define __SYSDEP_ARM64_KERNEL_OFFSETS_H

/*
 * Subarch contribution to arch/um/kernel/asm-offsets.c.
 *
 * arm64 needs nothing here. Guest TLS is TPIDR_EL0, which the guest writes
 * directly, so there is no descriptor table whose geometry the stub must know.
 */
#define ARCH_ASM_OFFSETS() do { } while (0)

#endif
