// SPDX-License-Identifier: GPL-2.0
/*
 * Exception-table fixup for ARCH=um SUBARCH=arm64.
 */
#include <arch.h>
#include <sysdep/ptrace.h>

/*
 * Declared here rather than included, because this file is built with
 * USER_CFLAGS against host headers and cannot see the kernel's definitions.
 * Mirrors arch/x86/um/fault.c.
 */
struct exception_table_entry {
	unsigned long insn;
	unsigned long fixup;
};

const struct exception_table_entry *search_exception_tables(unsigned long add);

int arch_fixup(unsigned long address, struct uml_pt_regs *regs)
{
	const struct exception_table_entry *fixup;

	fixup = search_exception_tables(address);
	if (fixup) {
		UPT_IP(regs) = fixup->fixup;
		return 1;
	}
	return 0;
}
