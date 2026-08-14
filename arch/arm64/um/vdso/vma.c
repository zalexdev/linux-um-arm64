// SPDX-License-Identifier: GPL-2.0-only
/*
 * Map the UML/arm64 vDSO into every guest process.
 *
 * Unlike x86-64 UML, where the vDSO is an optimisation, here it is load-bearing:
 * aarch64 glibc does not set SA_RESTORER, so returning from a guest signal
 * handler goes through __kernel_rt_sigreturn in this page. A guest without it
 * would crash on the first signal.
 */
#include <linux/slab.h>
#include <linux/sched.h>
#include <linux/mm.h>
#include <linux/init.h>
#include <linux/binfmts.h>
#include <asm/page.h>
#include <asm/elf.h>

#include "vdso-offsets.h"

unsigned long um_vdso_addr;
static struct page *um_vdso;

extern unsigned long task_size;
extern char vdso_start[], vdso_end[];

/*
 * Where a returning signal handler jumps to. Called from
 * setup_signal_stack_si() for every guest signal that does not carry
 * SA_RESTORER, which on aarch64 is essentially all of them.
 */
unsigned long um_vdso_sigreturn_addr(void)
{
	return um_vdso_addr + um_vdso_offset___kernel_rt_sigreturn;
}

static int __init init_vdso(void)
{
	BUG_ON(vdso_end - vdso_start > PAGE_SIZE);

	/*
	 * Immediately below the top of the guest address space, matching where
	 * the real arm64 kernel puts it and where x86 UML puts its own.
	 */
	um_vdso_addr = task_size - PAGE_SIZE;

	um_vdso = alloc_page(GFP_KERNEL);
	if (!um_vdso)
		panic("Cannot allocate vdso\n");

	copy_page(page_address(um_vdso), vdso_start);

	return 0;
}
subsys_initcall(init_vdso);

int arch_setup_additional_pages(struct linux_binprm *bprm, int uses_interp)
{
	struct vm_area_struct *vma;
	struct mm_struct *mm = current->mm;
	static struct vm_special_mapping vdso_mapping = {
		.name = "[vdso]",
		.pages = &um_vdso,
	};

	if (mmap_write_lock_killable(mm))
		return -EINTR;

	vma = _install_special_mapping(mm, um_vdso_addr, PAGE_SIZE,
				       VM_READ | VM_EXEC |
				       VM_MAYREAD | VM_MAYWRITE | VM_MAYEXEC,
				       &vdso_mapping);

	mmap_write_unlock(mm);

	return IS_ERR(vma) ? PTR_ERR(vma) : 0;
}
