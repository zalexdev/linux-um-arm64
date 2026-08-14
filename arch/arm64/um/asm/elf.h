/* SPDX-License-Identifier: GPL-2.0 */
#ifndef __UM_ARM64_ELF_H
#define __UM_ARM64_ELF_H

/*
 * arm64 has no <asm/user.h>: the ptrace register structures live in the uapi
 * ptrace header, and that is also where real arch/arm64 takes them from.
 */
#include <uapi/asm/ptrace.h>
#include <skas.h>

#define CORE_DUMP_USE_REGSET

/* aarch64 relocation types actually used by the module loader. */
#define R_AARCH64_NONE			0
#define R_AARCH64_ABS64			257
#define R_AARCH64_ABS32			258
#define R_AARCH64_ABS16			259
#define R_AARCH64_PREL64		260
#define R_AARCH64_PREL32		261
#define R_AARCH64_PREL16		262
#define R_AARCH64_LD_PREL_LO19		273
#define R_AARCH64_ADR_PREL_LO21		274
#define R_AARCH64_ADR_PREL_PG_HI21	275
#define R_AARCH64_ADD_ABS_LO12_NC	277
#define R_AARCH64_JUMP26		282
#define R_AARCH64_CALL26		283
#define R_AARCH64_GLOB_DAT		1025
#define R_AARCH64_JUMP_SLOT		1026
#define R_AARCH64_RELATIVE		1027

#define elf_check_arch(x) ((x)->e_machine == EM_AARCH64)

#define ELF_CLASS	ELFCLASS64
#define ELF_DATA	ELFDATA2LSB
#define ELF_ARCH	EM_AARCH64

/*
 * A fresh aarch64 process starts with x0..x30 zeroed. Zeroing them matters for
 * the same reason it does on x86: whatever the previous program left behind must
 * not leak into the new one, and glibc's startup reads x0 as the "rtld_fini"
 * argument, so a stale value there would be called as a function pointer.
 */
#define ELF_PLAT_INIT(regs, load_addr)					\
	do {								\
		int __i;						\
									\
		for (__i = HOST_X0; __i <= HOST_X30; __i++)		\
			PT_REGS_X(regs, __i) = 0;			\
	} while (0)

/*
 * NT_PRSTATUS layout for aarch64 is struct user_pt_regs itself: x0..x30, sp,
 * pc, pstate -- exactly the first 34 entries of gp[]. The two slots UML appends
 * past the end (orig_x0, syscall nr) are deliberately not dumped: they are not
 * part of the host ABI and a debugger reading the core would misinterpret them.
 */
/*
 * A plain compound statement, not do/while(0): include/linux/elfcore.h invokes
 * this without a trailing semicolon.
 */
#define ELF_CORE_COPY_REGS(pr_reg, _regs)				\
	{								\
		unsigned int __i;					\
									\
		for (__i = 0; __i < ELF_NGREG; __i++)			\
			(pr_reg)[__i] = (_regs)->regs.gp[__i];		\
	}

#define ELF_PLATFORM_FALLBACK "aarch64"

typedef unsigned long elf_greg_t;

#define ELF_NGREG (sizeof(struct user_pt_regs) / sizeof(elf_greg_t))
typedef elf_greg_t elf_gregset_t[ELF_NGREG];

typedef struct user_fpsimd_state elf_fpregset_t;

struct task_struct;

/*
 * PAGE_SIZE, not 4096: arm64 hosts ship 4K, 16K and 64K pages, and a UML guest's
 * page size always equals its host's. x86 UML hardcodes 4096 here because x86
 * has only ever had one page size.
 */
#define ELF_EXEC_PAGESIZE PAGE_SIZE

#define ELF_ET_DYN_BASE (2 * TASK_SIZE / 3)

/*
 * Sanitised in arch/arm64/um/cpuinfo.c: the guest is told only about features
 * whose architectural state UML actually preserves. Deliberately not the host's
 * raw AT_HWCAP -- see the allowlist there.
 */
extern unsigned long um_arm64_elf_hwcap;
extern unsigned long um_arm64_elf_hwcap2;
#define ELF_HWCAP (um_arm64_elf_hwcap)
#define ELF_HWCAP2 (um_arm64_elf_hwcap2)

extern char *elf_aux_platform;
#define ELF_PLATFORM (elf_aux_platform ?: ELF_PLATFORM_FALLBACK)

#define SET_PERSONALITY(ex) do {} while (0)

#define ARCH_HAS_SETUP_ADDITIONAL_PAGES 1
struct linux_binprm;
extern int arch_setup_additional_pages(struct linux_binprm *bprm,
				       int uses_interp);

/*
 * The guest finds the vDSO through AT_SYSINFO_EHDR, exactly as on real arm64.
 * glibc needs it to resolve __kernel_rt_sigreturn -- see arch/arm64/um/vdso/.
 */
extern unsigned long um_vdso_addr;
extern unsigned long um_vdso_sigreturn_addr(void);
#define AT_SYSINFO_EHDR 33
#define ARCH_DLINFO	NEW_AUX_ENT(AT_SYSINFO_EHDR, um_vdso_addr)

#endif /* __UM_ARM64_ELF_H */
