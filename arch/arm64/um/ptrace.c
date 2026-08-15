// SPDX-License-Identifier: GPL-2.0
/*
 * ptrace support for ARCH=um SUBARCH=arm64.
 *
 * The "user area" a debugger sees through PTRACE_PEEKUSR/POKEUSR is simply
 * struct user_pt_regs: x0..x30, sp, pc, pstate. That makes register lookup a
 * plain division rather than the offset table x86 needs, because arm64 has no
 * segment registers, no separate orig_ax slot and no debug-register block in
 * the user area.
 */
#include <linux/mm.h>
#include <linux/sched.h>
#include <linux/errno.h>
#include <linux/elf.h>
#include <linux/regset.h>
#include <linux/uaccess.h>
#include <asm/ptrace.h>
#include <registers.h>

static inline int reg_index(int regno)
{
	return regno / sizeof(unsigned long);
}

int putreg(struct task_struct *child, int regno, unsigned long value)
{
	int idx;

	if (regno < 0 || regno >= MAX_REG_OFFSET ||
	    (regno % sizeof(unsigned long)))
		return -EIO;

	idx = reg_index(regno);

	if (idx == HOST_PSTATE) {
		unsigned long old = child->thread.regs.regs.gp[HOST_PSTATE];

		child->thread.regs.regs.gp[HOST_PSTATE] =
			(old & ~UM_PSTATE_WRITABLE) | (value & UM_PSTATE_WRITABLE);
		return 0;
	}

	child->thread.regs.regs.gp[idx] = value;
	return 0;
}

unsigned long getreg(struct task_struct *child, int regno)
{
	if (regno < 0 || regno >= MAX_REG_OFFSET ||
	    (regno % sizeof(unsigned long)))
		return 0;

	return child->thread.regs.regs.gp[reg_index(regno)];
}

int poke_user(struct task_struct *child, long addr, long data)
{
	if ((addr & (sizeof(unsigned long) - 1)) || addr < 0)
		return -EIO;

	if (addr < MAX_REG_OFFSET)
		return putreg(child, addr, data);

	/*
	 * arm64 exposes no debug registers through the user area -- hardware
	 * breakpoints and watchpoints are separate regsets (NT_ARM_HW_BREAK,
	 * NT_ARM_HW_WATCH), which UML does not implement.
	 */
	return -EIO;
}

int peek_user(struct task_struct *child, long addr, long data)
{
	unsigned long tmp = 0;

	if ((addr & (sizeof(unsigned long) - 1)) || addr < 0)
		return -EIO;

	if (addr < MAX_REG_OFFSET)
		tmp = getreg(child, addr);

	return put_user(tmp, (unsigned long __user *) data);
}

static int genregs_get(struct task_struct *target,
		       const struct user_regset *regset,
		       struct membuf to)
{
	int reg;

	for (reg = 0; to.left; reg++)
		membuf_store(&to, getreg(target, reg * sizeof(unsigned long)));
	return 0;
}

static int genregs_set(struct task_struct *target,
		       const struct user_regset *regset,
		       unsigned int pos, unsigned int count,
		       const void *kbuf, const void __user *ubuf)
{
	int ret = 0;

	if (kbuf) {
		const unsigned long *k = kbuf;

		while (count >= sizeof(*k) && !ret) {
			ret = putreg(target, pos, *k++);
			count -= sizeof(*k);
			pos += sizeof(*k);
		}
	} else {
		const unsigned long __user *u = ubuf;

		while (count >= sizeof(*u) && !ret) {
			unsigned long word;

			ret = __get_user(word, u++);
			if (ret)
				break;
			ret = putreg(target, pos, word);
			count -= sizeof(*u);
			pos += sizeof(*u);
		}
	}
	return ret;
}

static int fpregs_active(struct task_struct *target,
			 const struct user_regset *regset)
{
	return regset->n;
}

static int fpregs_get(struct task_struct *target,
		      const struct user_regset *regset,
		      struct membuf to)
{
	void *fpregs = task_pt_regs(target)->regs.fp;

	membuf_write(&to, fpregs, regset->size * regset->n);
	return 0;
}

static int fpregs_set(struct task_struct *target,
		      const struct user_regset *regset,
		      unsigned int pos, unsigned int count,
		      const void *kbuf, const void __user *ubuf)
{
	void *fpregs = task_pt_regs(target)->regs.fp;

	return user_regset_copyin(&pos, &count, &kbuf, &ubuf,
				  fpregs, 0, regset->size * regset->n);
}

/*
 * NT_ARM_SYSTEM_CALL, without which a guest tracer cannot see or change the
 * syscall number at all.
 *
 * On arm64 the number is not part of the general register set -- x8 holds it on
 * entry but is not authoritative, and there is no orig_x8 -- so this regset is
 * the only interface for it. That makes it not an optional extra: without it,
 * PTRACE_GETREGSET(NT_ARM_SYSTEM_CALL) fails outright, and a tracer has no way
 * to name the syscall it is stopped in, or to cancel one by writing -1, which
 * is how every arm64 ptrace sandbox blocks a call.
 *
 * The storage is uml_pt_regs.syscall, which handle_syscall() fills before
 * calling the tracehooks and re-reads afterwards -- so a write here really does
 * redirect or cancel the syscall, as it does on real hardware.
 */
static int system_call_get(struct task_struct *target,
			   const struct user_regset *regset,
			   struct membuf to)
{
	int syscallno = PT_REGS_SYSCALL_NR(task_pt_regs(target));

	return membuf_store(&to, syscallno);
}

static int system_call_set(struct task_struct *target,
			   const struct user_regset *regset,
			   unsigned int pos, unsigned int count,
			   const void *kbuf, const void __user *ubuf)
{
	int syscallno = PT_REGS_SYSCALL_NR(task_pt_regs(target));
	int ret;

	ret = user_regset_copyin(&pos, &count, &kbuf, &ubuf, &syscallno, 0, -1);
	if (ret)
		return ret;

	PT_REGS_SYSCALL_NR(task_pt_regs(target)) = syscallno;
	return 0;
}

static struct user_regset uml_regsets[] __ro_after_init = {
	[REGSET_GENERAL] = {
		USER_REGSET_NOTE_TYPE(PRSTATUS),
		.n		= sizeof(struct user_pt_regs) / sizeof(long),
		.size		= sizeof(long),
		.align		= sizeof(long),
		.regset_get	= genregs_get,
		.set		= genregs_set,
	},
	[REGSET_FP] = {
		USER_REGSET_NOTE_TYPE(PRFPREG),
		/*
		 * .n is fixed up at boot from host_fp_size, because the host
		 * decides whether we are saving plain FPSIMD or an SVE state
		 * whose size depends on the vector length.
		 */
		.size		= sizeof(long),
		.align		= sizeof(long),
		.active		= fpregs_active,
		.regset_get	= fpregs_get,
		.set		= fpregs_set,
	},
	[REGSET_SYSTEM_CALL] = {
		USER_REGSET_NOTE_TYPE(ARM_SYSTEM_CALL),
		.n		= 1,
		.size		= sizeof(int),
		.align		= sizeof(int),
		.regset_get	= system_call_get,
		.set		= system_call_set,
	},
};

static const struct user_regset_view user_uml_view = {
	.name = "aarch64", .e_machine = EM_AARCH64,
	.regsets = uml_regsets, .n = ARRAY_SIZE(uml_regsets)
};

const struct user_regset_view *
task_user_regset_view(struct task_struct *tsk)
{
	return &user_uml_view;
}

/*
 * arm64 defines no architecture-specific ptrace requests that UML can serve.
 *
 * In particular there is no PTRACE_GETFPREGS/PTRACE_SETFPREGS: those are x86
 * and arm32 legacy requests, and arm64 deliberately never defined them --
 * floating-point state is read and written through PTRACE_GETREGSET with
 * NT_PRFPREG (or NT_ARM_SVE), which arch/um/kernel/ptrace.c already routes
 * through the regset view above. There is likewise no PTRACE_ARCH_PRCTL,
 * because arm64 has no arch_prctl.
 */
long subarch_ptrace(struct task_struct *child, long request,
		    unsigned long addr, unsigned long data)
{
	return -EIO;
}

static int __init init_regset_fp_info(void)
{
	uml_regsets[REGSET_FP].n =
		host_fp_size / uml_regsets[REGSET_FP].size;

	return 0;
}
arch_initcall(init_regset_fp_info);
