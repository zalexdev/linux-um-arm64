// SPDX-License-Identifier: GPL-2.0
/*
 * System call table for UML/arm64.
 *
 * arm64 uses the asm-generic syscall ABI, and its table is generated from
 * arch/arm64/tools/syscall_64.tbl into <asm/syscall_table_64.h>. The real arm64
 * kernel wraps every entry in an __arm64_sys_* trampoline that takes a
 * struct pt_regs *, because its entry path passes registers that way. UML does
 * not: handle_syscall() in arch/um/kernel/skas/syscall.c unpacks the six
 * arguments itself and calls the syscall with them directly, exactly as x86 UML
 * does. So the table here holds the plain sys_* functions and the pt_regs
 * wrappers are neither built nor needed.
 */
#include <linux/linkage.h>
#include <linux/sys.h>
#include <linux/cache.h>
#include <asm/syscall.h>
#include <asm/unistd.h>

extern asmlinkage long sys_ni_syscall(unsigned long, unsigned long,
				      unsigned long, unsigned long,
				      unsigned long, unsigned long);

/*
 * A UML guest is always 64-bit; there is no compat (AArch32) execution, so the
 * compat half of every dual entry is simply dropped.
 */
#define __SYSCALL_WITH_COMPAT(nr, native, compat) __SYSCALL(nr, native)

#define __SYSCALL(nr, sym) extern asmlinkage long sym(unsigned long, unsigned long, unsigned long, unsigned long, unsigned long, unsigned long);
#include <asm/syscall_table_64.h>
#undef  __SYSCALL

#define __SYSCALL(nr, sym) [nr] = sym,
const sys_call_ptr_t sys_call_table[__NR_syscalls] ____cacheline_aligned = {
	[0 ... __NR_syscalls - 1] = sys_ni_syscall,
#include <asm/syscall_table_64.h>
};

int syscall_table_size = sizeof(sys_call_table);
