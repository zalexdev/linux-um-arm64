/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * Module linker script additions for ARCH=um SUBARCH=arm64.
 *
 * module_frob_arch_sections() in arch/arm64/kernel/module-plts.c counts the
 * relocations that may need a veneer and sizes .plt and .init.plt accordingly.
 * It finds those sections by name, so they have to exist in every module --
 * which is what this script guarantees. Without it the sections are absent
 * rather than empty, module_frob_arch_sections() has nowhere to put the PLT,
 * and any module whose branches exceed the +/-128 MiB range of a direct BL
 * fails to load.
 *
 * The ftrace trampoline sections are declared for the same reason arm64
 * declares them: the shared module.c looks for them under
 * CONFIG_DYNAMIC_FTRACE. UML has no arm64 ftrace support today, so they stay
 * empty, and an empty section costs nothing.
 *
 * Deliberately shorter than arch/arm64/include/asm/module.lds.h: the KASAN_SW_TAGS
 * and UNWIND_TABLES clauses there depend on options that arch/arm64/Kconfig
 * offers and ARCH=um does not, since that Kconfig is not sourced here.
 */
SECTIONS {
	.plt 0 : { BYTE(0) }
	.init.plt 0 : { BYTE(0) }
	.text.ftrace_trampoline 0 : { BYTE(0) }
	.init.text.ftrace_trampoline 0 : { BYTE(0) }
}
