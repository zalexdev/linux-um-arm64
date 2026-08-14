/* SPDX-License-Identifier: GPL-2.0 */
#ifndef __UM_ARM64_STRING_H
#define __UM_ARM64_STRING_H

/*
 * arch/arm64/include/asm/string.h declares hand-written assembly versions of
 * strrchr, strchr, strcmp, memcpy and friends, implemented in arch/arm64/lib.
 * Those are kernel-only routines and arch/arm64/lib is not built for ARCH=um,
 * so declaring __HAVE_ARCH_* here would suppress lib/string.c's generic C
 * versions and leave the symbols undefined at link time.
 *
 * Defining no __HAVE_ARCH_* at all lets lib/string.c supply everything. That
 * also keeps UML's -Dstrrchr=kernel_strrchr renaming (see arch/um/Makefile)
 * working, which is what stops the kernel's string functions from colliding with
 * the libc that the UML binary is linked against.
 */

#endif /* __UM_ARM64_STRING_H */
