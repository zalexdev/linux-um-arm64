// SPDX-License-Identifier: GPL-2.0
/*
 * mcontext <-> uml_pt_regs conversion for ARCH=um SUBARCH=arm64.
 *
 * This is host-side code: it manipulates the signal frame the *host* built for
 * a stub process. aarch64 makes this markedly simpler than x86:
 *
 *   - the faulting address is a first-class member of mcontext_t, so there is
 *     nothing to dig out of a gregs[] array;
 *   - the FP state lives inline in mcontext.__reserved[] rather than behind a
 *     pointer into a separate area, so it is already inside the sigstack copy
 *     UML has in hand and needs no address translation;
 *   - there are no segment registers to reassemble.
 */
#include <linux/errno.h>
#include <linux/string.h>
#include <sys/ucontext.h>
#include <sysdep/ptrace.h>
#include <sysdep/mcontext.h>
#include <sysdep/stub-data.h>
#include <stub-data.h>
#include <arch.h>

void get_regs_from_mc(struct uml_pt_regs *regs, mcontext_t *mc)
{
	int i;

	for (i = 0; i < 31; i++)
		regs->gp[HOST_X0 + i] = mc->regs[i];
	regs->gp[HOST_SP] = mc->sp;
	regs->gp[HOST_PC] = mc->pc;
	regs->gp[HOST_PSTATE] = mc->pstate;

	/*
	 * x8 holds the syscall number at the SVC that trapped into seccomp.
	 * Unlike the ptrace path there is no NT_ARM_SYSTEM_CALL to consult here
	 * -- the stub is stopped in its own SIGSYS handler, not in a
	 * syscall-entry stop -- and at that point x8 is authoritative.
	 */
	regs->gp[HOST_SYSCALL_NR] = mc->regs[8];
	regs->gp[HOST_ORIG_X0] = mc->regs[0];
}

void get_mc_from_regs(struct uml_pt_regs *regs, mcontext_t *mc,
		      int single_stepping)
{
	int i;

	for (i = 0; i < 31; i++)
		mc->regs[i] = regs->gp[HOST_X0 + i];
	mc->sp = regs->gp[HOST_SP];
	mc->pc = regs->gp[HOST_PC];
	mc->pstate = regs->gp[HOST_PSTATE];

	/*
	 * single_stepping is ignored, and must be.
	 *
	 * x86 implements it here by setting EFLAGS.TF in the frame the stub is
	 * about to sigreturn to. arm64's equivalent is PSTATE.SS, which only has
	 * an effect when MDSCR_EL1.SS is also set -- and MDSCR_EL1 is an EL1
	 * register that a userspace stub cannot write. There is no way to arm a
	 * single step from inside the stub.
	 *
	 * Single-stepping a guest therefore requires the ptrace path, where
	 * PTRACE_SINGLESTEP asks the host kernel to do it. arch/um/kernel/ptrace.c
	 * puts a task that wants single-stepping onto that path; nothing is
	 * silently dropped here.
	 */
}

void mc_set_rip(void *_mc, void *target)
{
	mcontext_t *mc = _mc;

	mc->pc = (unsigned long)target;
}

/*
 * Locate a record in the mcontext's __reserved chain.
 *
 * Bounded on both the individual record sizes and the buffer length: this walks
 * a structure that lives in memory the guest process could have scribbled on.
 */
static void *find_ctx(mcontext_t *mc, unsigned int magic, unsigned int *size)
{
	unsigned char *p = (unsigned char *)mc->__reserved;
	unsigned long left = sizeof(mc->__reserved);
	unsigned long off = 0;

	while (off + sizeof(struct um_aarch64_ctx) <= left) {
		struct um_aarch64_ctx *h = (struct um_aarch64_ctx *)(p + off);

		if (h->magic == 0 || h->size == 0)
			return NULL;
		if (h->size > left - off)
			return NULL;
		if (h->magic == magic) {
			if (size)
				*size = h->size;
			return h;
		}
		off += h->size;
	}
	return NULL;
}

/*
 * FPSIMD record layout, spelled out rather than taken from <asm/sigcontext.h>
 * so this file does not depend on which host headers happen to be installed.
 * Matches struct fpsimd_context in the arm64 uapi.
 */
struct um_fpsimd_context {
	struct um_aarch64_ctx head;
	unsigned int fpsr;
	unsigned int fpcr;
	__uint128_t vregs[32];
};

int get_stub_state(struct uml_pt_regs *regs, struct stub_data *data,
		   unsigned long *fp_size_out)
{
	struct um_fpsimd_context *fp;
	unsigned int size = 0;
	mcontext_t *mcontext;

	/* mctx_offset is verified by wait_stub_done_seccomp */
	mcontext = (void *)&data->sigstack[data->mctx_offset];

	get_regs_from_mc(regs, mcontext);

	/*
	 * TPIDR_EL0 is not in the mcontext -- the host does not save it in a
	 * signal frame, because on a real kernel it is per-thread state the
	 * scheduler restores rather than something a handler can change. Under
	 * UML the guest's threads all live in one stub process, so UML has to
	 * track it, and the stub captured it for us on the way in.
	 *
	 * Reading it back rather than assuming it matters because the guest can
	 * write TPIDR_EL0 itself with a single unprivileged "msr"; glibc and musl
	 * both do exactly that when they set up the initial thread. See
	 * stub_seccomp_save_state().
	 */
	regs->gp[HOST_TLS] = data->arch_data.tls;

	fp = find_ctx(mcontext, UM_FPSIMD_MAGIC, &size);
	if (!fp || size < sizeof(*fp))
		return -EINVAL;

	if (fp_size_out)
		*fp_size_out = sizeof(fp->vregs) + sizeof(fp->fpsr) +
			       sizeof(fp->fpcr);

	if (sizeof(fp->vregs) + sizeof(fp->fpsr) + sizeof(fp->fpcr) >
	    host_fp_size)
		return -ENOSPC;

	/*
	 * regs->fp mirrors the host's NT_PRFPREG / NT_ARM_SVE layout, which
	 * begins with the 32 V registers followed by fpsr and fpcr -- the same
	 * order as struct user_fpsimd_state, and the reverse of the order inside
	 * fpsimd_context, hence the two copies rather than one.
	 */
	memcpy(regs->fp, fp->vregs, sizeof(fp->vregs));
	memcpy((unsigned char *)regs->fp + sizeof(fp->vregs), &fp->fpsr,
	       sizeof(fp->fpsr));
	memcpy((unsigned char *)regs->fp + sizeof(fp->vregs) + sizeof(fp->fpsr),
	       &fp->fpcr, sizeof(fp->fpcr));

	return 0;
}

int set_stub_state(struct uml_pt_regs *regs, struct stub_data *data,
		   int single_stepping)
{
	struct um_fpsimd_context *fp;
	unsigned int size = 0;
	mcontext_t *mcontext;

	/* mctx_offset is verified by wait_stub_done_seccomp */
	mcontext = (void *)&data->sigstack[data->mctx_offset];

	if ((unsigned long)mcontext < (unsigned long)data->sigstack ||
	    (unsigned long)mcontext >
			(unsigned long)data->sigstack +
			sizeof(data->sigstack) - sizeof(*mcontext))
		return -EINVAL;

	get_mc_from_regs(regs, mcontext, single_stepping);

	fp = find_ctx(mcontext, UM_FPSIMD_MAGIC, &size);
	if (!fp || size < sizeof(*fp))
		return -EINVAL;

	memcpy(fp->vregs, regs->fp, sizeof(fp->vregs));
	memcpy(&fp->fpsr, (unsigned char *)regs->fp + sizeof(fp->vregs),
	       sizeof(fp->fpsr));
	memcpy(&fp->fpcr,
	       (unsigned char *)regs->fp + sizeof(fp->vregs) + sizeof(fp->fpsr),
	       sizeof(fp->fpcr));

	/*
	 * Guest TLS. x86-64 has to hand FS_BASE/GS_BASE to the stub because they
	 * are only settable via arch_prctl; arm64's TPIDR_EL0 is writable at
	 * EL0, so all that is needed is to tell the stub the value and let it
	 * install it with a single "msr" on resume.
	 */
	if (data->arch_data.tls != regs->gp[HOST_TLS]) {
		data->arch_data.tls = regs->gp[HOST_TLS];
		data->arch_data.sync |= STUB_SYNC_TLS;
	}

	return 0;
}
