/* SPDX-License-Identifier: GPL-2.0 */
#ifndef __VM_FLAGS_ARM64_H
#define __VM_FLAGS_ARM64_H

/*
 * Matches arch/arm64/include/asm/page.h: data mappings are executable only if
 * the task's personality says so (READ_IMPLIES_EXEC), never unconditionally.
 * The stack is left at the generic default, i.e. non-executable, which is the
 * arm64 ABI -- x86-64 UML overrides it to executable, and copying that here
 * would silently give every UML/arm64 guest an executable stack.
 */
#define VMA_DATA_DEFAULT_FLAGS	VMA_DATA_FLAGS_TSK_EXEC

#endif
