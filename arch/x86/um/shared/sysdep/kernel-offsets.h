/* SPDX-License-Identifier: GPL-2.0 */
#ifndef __SYSDEP_X86_KERNEL_OFFSETS_H
#define __SYSDEP_X86_KERNEL_OFFSETS_H

#include <asm/alternative.h>
#include <asm/extable.h>
#include <asm/segment.h>

/*
 * Subarch contribution to arch/um/kernel/asm-offsets.c.
 *
 * The GDT TLS entry count is meaningful only where there is a GDT. It used to
 * be emitted unconditionally from generic code, which also forced
 * <asm/segment.h> into arch/um/include/asm/thread_info.h, where nothing used it.
 */
/*
 * ALT_INSTR_SIZE and EXTABLE_SIZE are referenced only from x86's
 * <asm/alternative.h> and <asm/asm.h>, where inline asm builds .altinstructions
 * and __ex_table entries. Nothing outside x86 consumes them.
 */
#define ARCH_ASM_OFFSETS()						\
	do {								\
		DEFINE(UM_KERN_GDT_ENTRY_TLS_ENTRIES,			\
		       GDT_ENTRY_TLS_ENTRIES);				\
		DEFINE(ALT_INSTR_SIZE, sizeof(struct alt_instr));	\
		DEFINE(EXTABLE_SIZE,					\
		       sizeof(struct exception_table_entry));		\
	} while (0)

#endif
