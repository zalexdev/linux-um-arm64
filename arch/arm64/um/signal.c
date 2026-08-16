// SPDX-License-Identifier: GPL-2.0
/*
 * Guest signal delivery for ARCH=um SUBARCH=arm64.
 *
 * The frame built here is consumed by guest userspace, so it must match the
 * arm64 signal ABI exactly -- glibc, musl and every unwinder parse it. It is
 * laid out the same way arch/arm64/kernel/signal.c lays it out:
 *
 *	 sp ->	struct rt_sigframe { siginfo info; struct ucontext uc; }
 *		struct frame_record { u64 fp; u64 lr; }		<- for unwinders
 *
 * and on entry to the handler:
 *
 *	x0 = signo, x1 = &info, x2 = &uc
 *	sp = frame, x29 = &frame_record, x30 = return trampoline
 *	pc = handler
 *
 * uc.uc_mcontext.__reserved[] carries a chain of _aarch64_ctx records. UML
 * writes an fpsimd_context followed by a null terminator. It deliberately does
 * *not* write an sve_context: UML does not virtualise SVE for the guest, and
 * emitting a record whose vector length came from the host would tell guest
 * userspace it may use vectors the guest kernel never saves.
 */
#include <linux/personality.h>
#include <linux/ptrace.h>
#include <linux/kernel.h>
#include <linux/syscalls.h>
#include <linux/uaccess.h>
#include <linux/regset.h>
#include <asm/unistd.h>
#include <asm/ucontext.h>
#include <asm/sigcontext.h>
#include <frame_kern.h>
#include <registers.h>
#include <skas.h>

struct rt_sigframe {
	struct siginfo info;
	struct ucontext uc;
};

/*
 * Pushed below the frame so an unwinder walking x29 can step out of the signal
 * handler. arm64 requires the pair to be 16-byte aligned.
 */
struct frame_record {
	u64 fp;
	u64 lr;
};

/*
 * Move FP state between the guest's register file and a signal frame.
 *
 * These two structures order their fields differently and it is not a detail
 * that can be papered over with one memcpy:
 *
 *   struct user_fpsimd_state (what regs->fp holds, i.e. the host's NT_PRFPREG
 *                             layout):     { vregs[32], fpsr, fpcr }
 *   struct fpsimd_context    (what goes in mcontext.__reserved[], i.e. the
 *                             guest-visible signal ABI):
 *                                          { head, fpsr, fpcr, vregs[32] }
 *
 * Copying vregs+fpsr+fpcr as one block from the start of one into the vregs of
 * the other transfers the vector registers correctly and silently drops FPCR
 * and FPSR -- which is invisible to anything that does not check the control
 * registers after taking a signal, and shows up much later as a guest whose
 * rounding mode or denormal handling resets at random.
 */
/*
 * The bytes these two functions actually move.
 *
 * Not sizeof(struct user_fpsimd_state): that is eight bytes larger, because
 * the uapi struct carries a __reserved[2] tail which neither the host's
 * NT_PRFPREG payload nor a signal frame's fpsimd_context contains, and which
 * nothing here ever reads or writes.
 *
 * Guarding on the struct size instead of on this was a silent disaster in
 * SECCOMP mode. There host_fp_size is derived from the mcontext and comes out
 * as exactly vregs + fpsr + fpcr = 520, so "host_fp_size < sizeof(*st)" is
 * 520 < 528 -- true -- and both directions returned without copying anything.
 * The guest's vector registers were therefore never saved into a signal frame
 * and never restored from one: any guest signal handler that touched a V
 * register silently corrupted the state of the code it interrupted.
 *
 * That is not a corner case. Go preempts goroutines with SIGURG at arbitrary
 * instructions from 1.14 onwards, and its arm64 AES-GCM assembly holds cipher
 * state across most of the register file, so every TLS transfer past a few
 * megabytes died with "tls: bad record MAC" -- which Docker reports as
 * "filesystem layer verification failed", pointing at the disk. The ptrace
 * path was unaffected, because there host_fp_size is the NT_PRFPREG regset
 * size, which is sizeof(*st).
 */
#define UM_FP_PAYLOAD_SIZE offsetof(struct user_fpsimd_state, __reserved)

static void fpstate_to_sigctx(struct fpsimd_context *fp, struct pt_regs *regs)
{
	struct user_fpsimd_state *st = (struct user_fpsimd_state *)regs->regs.fp;

	if (host_fp_size < UM_FP_PAYLOAD_SIZE)
		return;

	memcpy(&fp->vregs, &st->vregs, sizeof(fp->vregs));
	fp->fpsr = st->fpsr;
	fp->fpcr = st->fpcr;
}

static void sigctx_to_fpstate(struct pt_regs *regs, const struct fpsimd_context *fp)
{
	struct user_fpsimd_state *st = (struct user_fpsimd_state *)regs->regs.fp;

	if (host_fp_size < UM_FP_PAYLOAD_SIZE)
		return;

	memcpy(&st->vregs, &fp->vregs, sizeof(fp->vregs));
	st->fpsr = fp->fpsr;
	st->fpcr = fp->fpcr;
}

/*
 * The vDSO's sigreturn trampoline (arch/arm64/um/vdso/sigreturn.S) describes
 * where the interrupted registers live using CFI offsets it cannot compute --
 * they are literals in a .S file. If the frame layout here ever moves, those
 * literals become silently wrong, and the only symptom is that gdb and
 * libunwind produce garbage backtraces out of signal handlers, which nothing
 * else in the test suite would notice. Pin them.
 */
static void __always_unused um_sigframe_abi_check(void)
{
	BUILD_BUG_ON(sizeof(struct siginfo) != 128);
	BUILD_BUG_ON(offsetof(struct rt_sigframe, info) != 0);
	BUILD_BUG_ON(offsetof(struct rt_sigframe, uc) != 128);
	BUILD_BUG_ON(offsetof(struct ucontext, uc_mcontext) != 176);
	BUILD_BUG_ON(offsetof(struct sigcontext, regs) != 8);
	/* SIGFRAME_REGS_OFF in sigreturn.S is the sum of the three above. */
	BUILD_BUG_ON(128 + 176 + 8 != 312);
	/* The frame_record must keep the stack 16-byte aligned. */
	BUILD_BUG_ON(sizeof(struct frame_record) % 16 != 0);
}

static int copy_sc_from_user(struct pt_regs *regs,
			     struct sigcontext __user *from)
{
	struct sigcontext sc;
	struct fpsimd_context fpsimd;
	struct _aarch64_ctx head;
	unsigned long offset = 0;
	int i, err;

	/* Any restarted syscall interrupted by this signal now returns -EINTR. */
	current->restart_block.fn = do_no_restart_syscall;

	err = copy_from_user(&sc, from, sizeof(sc));
	if (err)
		return err;

	for (i = 0; i < 31; i++)
		regs->regs.gp[HOST_X0 + i] = sc.regs[i];
	regs->regs.gp[HOST_SP] = sc.sp;
	regs->regs.gp[HOST_PC] = sc.pc;

	/*
	 * PSTATE is filtered the same way arch/arm64/kernel/ptrace.c filters it
	 * for a debugger. Restoring it verbatim from a user-writable buffer
	 * would let the guest hand itself interrupt-masked or non-EL0t state.
	 */
	regs->regs.gp[HOST_PSTATE] =
		(regs->regs.gp[HOST_PSTATE] & ~UM_PSTATE_WRITABLE) |
		(sc.pstate & UM_PSTATE_WRITABLE);

	/*
	 * Walk the __reserved chain for the FPSIMD record. A missing or
	 * malformed chain is not fatal: the guest simply keeps whatever FP state
	 * it had, which is what the hardware would do if the record were absent.
	 */
	while (offset + sizeof(head) <= sizeof(sc.__reserved)) {
		memcpy(&head, &sc.__reserved[offset], sizeof(head));
		if (head.magic == 0 || head.size == 0)
			break;
		if (head.size > sizeof(sc.__reserved) - offset)
			break;
		if (head.magic == FPSIMD_MAGIC) {
			if (head.size < sizeof(fpsimd))
				break;
			memcpy(&fpsimd, &sc.__reserved[offset], sizeof(fpsimd));
			sigctx_to_fpstate(regs, &fpsimd);
			break;
		}
		offset += head.size;
	}

	return 0;
}

static int copy_sc_to_user(struct sigcontext __user *to, struct pt_regs *regs,
			   unsigned long fault_address)
{
	struct sigcontext sc;
	struct fpsimd_context *fpsimd;
	struct _aarch64_ctx *term;
	int i;

	memset(&sc, 0, sizeof(sc));

	sc.fault_address = fault_address;
	for (i = 0; i < 31; i++)
		sc.regs[i] = regs->regs.gp[HOST_X0 + i];
	sc.sp = regs->regs.gp[HOST_SP];
	sc.pc = regs->regs.gp[HOST_PC];
	sc.pstate = regs->regs.gp[HOST_PSTATE];

	BUILD_BUG_ON(sizeof(struct fpsimd_context) +
		     sizeof(struct _aarch64_ctx) > sizeof(sc.__reserved));

	fpsimd = (struct fpsimd_context *)&sc.__reserved[0];
	fpsimd->head.magic = FPSIMD_MAGIC;
	fpsimd->head.size = sizeof(*fpsimd);
	fpstate_to_sigctx(fpsimd, regs);

	/* Null terminator, so a parser knows where the chain ends. */
	term = (struct _aarch64_ctx *)&sc.__reserved[sizeof(*fpsimd)];
	term->magic = 0;
	term->size = 0;

	return copy_to_user(to, &sc, sizeof(sc));
}

int setup_signal_stack_si(unsigned long stack_top, struct ksignal *ksig,
			  struct pt_regs *regs, sigset_t *mask)
{
	struct rt_sigframe __user *frame;
	struct frame_record __user *rec;
	unsigned long sp;
	int err = 0;

	/* AAPCS64 requires a 16-byte aligned stack pointer at all times. */
	sp = round_down(stack_top, 16);

	sp -= sizeof(struct frame_record);
	rec = (struct frame_record __user *)sp;

	sp = round_down(sp - sizeof(*frame), 16);
	frame = (struct rt_sigframe __user *)sp;

	if (!access_ok(frame, sizeof(*frame)) || !access_ok(rec, sizeof(*rec)))
		return 1;

	err |= copy_siginfo_to_user(&frame->info, &ksig->info);

	err |= __put_user(0, &frame->uc.uc_flags);
	err |= __put_user(NULL, &frame->uc.uc_link);
	err |= __save_altstack(&frame->uc.uc_stack, PT_REGS_SP(regs));
	err |= copy_sc_to_user(&frame->uc.uc_mcontext, regs,
			       current->thread.arch.faultinfo.addr);
	err |= copy_to_user(&frame->uc.uc_sigmask, mask, sizeof(*mask));

	err |= __put_user(regs->regs.gp[HOST_X29], &rec->fp);
	err |= __put_user(regs->regs.gp[HOST_X30], &rec->lr);

	if (err)
		return err;

	PT_REGS_SP(regs) = (unsigned long) frame;
	PT_REGS_IP(regs) = (unsigned long) ksig->ka.sa.sa_handler;
	PT_REGS_X0(regs) = (unsigned long) ksig->sig;
	PT_REGS_X1(regs) = (unsigned long) &frame->info;
	PT_REGS_X2(regs) = (unsigned long) &frame->uc;
	PT_REGS_X29(regs) = (unsigned long) rec;

	/*
	 * Where the handler returns to.
	 *
	 * arm64 has no in-frame trampoline: the stack is not executable, so
	 * unlike i386 UML there is nowhere in the frame to put "svc #0". Real
	 * arm64 always returns into the vDSO's __kernel_rt_sigreturn unless the
	 * guest supplied SA_RESTORER. UML/arm64 provides the same vDSO symbol;
	 * see arch/arm64/um/vdso/.
	 */
	if (ksig->ka.sa.sa_flags & SA_RESTORER)
		PT_REGS_X30(regs) = (unsigned long) ksig->ka.sa.sa_restorer;
	else
		PT_REGS_X30(regs) = um_vdso_sigreturn_addr();

	return 0;
}

/*
 * arm64 has no non-realtime signal frame: sigaction(2) without SA_SIGINFO still
 * delivers an rt frame, because the architecture was defined after that
 * distinction stopped mattering. arch/um/kernel/signal.c only calls this when
 * !SA_SIGINFO, so forward it.
 */
int setup_signal_stack_sc(unsigned long stack_top, struct ksignal *ksig,
			  struct pt_regs *regs, sigset_t *mask)
{
	return setup_signal_stack_si(stack_top, ksig, regs, mask);
}

SYSCALL_DEFINE0(rt_sigreturn)
{
	unsigned long sp = PT_REGS_SP(&current->thread.regs);
	struct rt_sigframe __user *frame = (struct rt_sigframe __user *)sp;
	struct ucontext __user *uc = &frame->uc;
	sigset_t set;

	/*
	 * The frame was placed at a 16-byte boundary and the handler was entered
	 * with sp pointing at it, so a misaligned sp means the guest corrupted
	 * its stack rather than returning from a signal.
	 */
	if (sp & 15)
		goto segfault;

	if (copy_from_user(&set, &uc->uc_sigmask, sizeof(set)))
		goto segfault;

	set_current_blocked(&set);

	if (copy_sc_from_user(&current->thread.regs, &uc->uc_mcontext))
		goto segfault;

	if (restore_altstack(&uc->uc_stack))
		goto segfault;

	/*
	 * Do not let the syscall restart machinery act on this return value.
	 *
	 * It is PT_REGS_SYSCALL_NR that has to be cleared, not
	 * PT_REGS_ORIG_SYSCALL. arch/um/kernel/signal.c tests
	 * "PT_REGS_SYSCALL_NR(regs) >= 0" to decide whether a syscall is in
	 * flight, and that is uml_pt_regs.syscall -- a different field.
	 *
	 * x86 writes PT_REGS_SYSCALL_NR here and can write nothing else,
	 * because there ORIG_SYSCALL aliases AX, which is also the return
	 * register. On arm64 the two were deliberately separated, ORIG_SYSCALL
	 * onto a synthetic slot, so translating the line by name moved it to
	 * different storage and left uml_pt_regs.syscall holding
	 * __NR_rt_sigreturn.
	 *
	 * The consequence is silent: a handler returning into a context whose
	 * x0 happens to hold one of the four -ERESTART* values, with another
	 * signal deliverable, gets its context rewritten -- the program counter
	 * rewound four bytes and x0 replaced from a slot sigreturn never
	 * restored. Rare enough that 550 clean gate runs did not hit it, and
	 * entirely invisible when it does.
	 */
	PT_REGS_SYSCALL_NR(&current->thread.regs) = -1;
	return PT_REGS_SYSCALL_RET(&current->thread.regs);

 segfault:
	force_sig(SIGSEGV);
	return 0;
}
