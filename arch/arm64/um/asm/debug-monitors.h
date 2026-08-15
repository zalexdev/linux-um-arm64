/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * ARCH=um SUBARCH=arm64 has no hardware debug monitors: a UML guest is a
 * userspace process and never programs MDSCR_EL1, breakpoints or watchpoints.
 *
 * This exists only so the reused arch/arm64/lib/insn.c can include
 * <asm/debug-monitors.h>. It needs nothing from it -- the one constant it uses,
 * AARCH64_BREAK_FAULT, comes from <asm/insn-def.h> -- but arch/arm64's copy
 * reaches asm/sysreg.h and the generated asm/sysreg-defs.h, which are not built
 * for ARCH=um.
 */
#ifndef __UM_ARM64_DEBUG_MONITORS_H
#define __UM_ARM64_DEBUG_MONITORS_H

#endif /* __UM_ARM64_DEBUG_MONITORS_H */
