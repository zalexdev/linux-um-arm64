/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * Module loading for ARCH=um SUBARCH=arm64.
 *
 * The loader itself is the parent architecture's: arch/arm64/kernel/module.c
 * and module-plts.c are built into UML unchanged (see arch/arm64/um/Makefile),
 * because relocation types, PLT veneers and instruction encodings are
 * properties of the aarch64 ELF ABI and of the instruction set rather than of
 * how the kernel is hosted.
 *
 * This header exists because <asm/module.h> does not reach the parent
 * architecture's copy under ARCH=um -- UML's own asm/ generic wrapper wins the
 * include path -- and because two of the declarations there mean something
 * different here.
 */
#ifndef __UM_ARM64_MODULE_H
#define __UM_ARM64_MODULE_H

#include <asm-generic/module.h>
#include <asm/cpufeature.h>

/*
 * Number of PLT slots the ftrace trampoline sections are sized for.
 * arch/arm64 defines this in <asm/ftrace.h>, which ARCH=um supplies its own
 * copy of; module-plts.c uses it unconditionally when sizing those sections,
 * so it has to be defined even though UML has no arm64 ftrace and the
 * sections stay empty.
 */
#ifndef NR_FTRACE_PLTS
#define NR_FTRACE_PLTS 1
#endif

struct mod_plt_sec {
	int			plt_shndx;
	int			plt_num_entries;
	int			plt_max_entries;
};

struct mod_arch_specific {
	struct mod_plt_sec	core;
	struct mod_plt_sec	init;

#ifdef CONFIG_DYNAMIC_FTRACE
	/*
	 * Declared for symmetry with arch/arm64. UML has no arm64 ftrace
	 * support, so CONFIG_DYNAMIC_FTRACE cannot currently be set here; the
	 * fields are kept so the shared module.c continues to compile if it
	 * ever can be.
	 */
	struct plt_entry	*ftrace_trampolines;
	struct plt_entry	*init_ftrace_trampolines;
#endif
};

u64 module_emit_plt_entry(struct module *mod, Elf64_Shdr *sechdrs,
			  void *loc, const Elf64_Rela *rela,
			  Elf64_Sym *sym);

u64 module_emit_veneer_for_adrp(struct module *mod, Elf64_Shdr *sechdrs,
				void *loc, u64 val);

struct plt_entry {
	/*
	 * A program that conforms to the AArch64 Procedure Call Standard
	 * (AAPCS64) must assume that a veneer that alters IP0 (x16) and/or
	 * IP1 (x17) may be inserted at any branch instruction that is
	 * exposed to a relocation that supports long branches. Since that
	 * is exactly what we are dealing with here, we are free to use x16
	 * as a scratch register in the PLT veneers.
	 */
	__le32	adrp;	/* adrp	x16, ....			*/
	__le32	add;	/* add	x16, x16, #0x....		*/
	__le32	br;	/* br	x16				*/
};

/*
 * On real hardware this reports the Cortex-A53 erratum 843419 workaround, which
 * forbids placing an ADRP in the last 8 bytes of a 4 KiB page, and it is driven
 * by a CPU capability bit discovered at boot.
 *
 * UML has no cpucaps: it is an ordinary userspace process and does not probe
 * CPU errata, and CONFIG_ARM64_ERRATUM_843419 is not offered here because
 * arch/arm64/Kconfig is not sourced for ARCH=um. Reporting "not forbidden" is
 * therefore the honest answer to a question this configuration cannot ask, and
 * it is what the same code does on hardware whenever the capability is absent.
 *
 * Recorded as a limitation rather than a fix: a UML guest running on an
 * affected Cortex-A53 would not get the veneer that hardware would. That is the
 * same exposure as any other userspace program on such a part, since the guest
 * executes at EL0 either way.
 */
static inline bool is_forbidden_offset_for_adrp(void *place)
{
	return false;
}

struct plt_entry get_plt_entry(u64 dst, void *pc);

static inline const Elf_Shdr *find_section(const Elf_Ehdr *hdr,
				    const Elf_Shdr *sechdrs,
				    const char *name)
{
	const Elf_Shdr *s, *se;
	const char *secstrs = (void *)hdr + sechdrs[hdr->e_shstrndx].sh_offset;

	for (s = sechdrs, se = sechdrs + hdr->e_shnum; s < se; s++) {
		if (strcmp(name, secstrs + s->sh_name) == 0)
			return s;
	}

	return NULL;
}

#endif /* __UM_ARM64_MODULE_H */
