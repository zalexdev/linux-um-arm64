// SPDX-License-Identifier: GPL-2.0
/*
 * Can UML get off an arm64 syscall stop onto one where x7 is usable?
 *
 * hostx7.c establishes the problem: at a ptrace syscall stop arm64 replaces x7
 * with the stop direction and discards anything the tracer writes there. UML
 * multiplexes every thread of a guest mm onto one traced stub process, so it
 * must be able to install a different thread's x7 -- and at a syscall stop it
 * cannot.
 *
 * This measures the escape route. Reading arch/arm64/kernel/syscall.c suggests
 * that resuming a PTRACE_SYSEMU entry stop with PTRACE_SINGLESTEP lands on a
 * *pseudo-step* trap rather than a syscall stop, because syscall_trace_exit()
 * re-reads the thread flags and takes the TIF_SINGLESTEP branch of
 * report_syscall_exit(), which restores x7 before reporting. If that is right
 * then the escape costs no guest instructions at all: the syscall is still
 * skipped (the emulation decision was taken from the flags read before the
 * stop), the program counter does not move, and the tracer arrives at a stop
 * where x7 is both readable and writable.
 *
 * Every one of those clauses is checked below, because the fix in
 * arch/um/os-Linux/skas/process.c depends on all of them and none of them are
 * things a comment in a source file should be trusted for.
 */
#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>
#include <signal.h>
#include <sys/ptrace.h>
#include <sys/wait.h>
#include <sys/uio.h>
#include <linux/elf.h>

#ifndef PTRACE_SYSEMU
#define PTRACE_SYSEMU 31
#endif

#define MAGIC	0x1234567890abcdefUL
#define NEWX7	0x00c0ffee0badf00dUL

static int getregs(int pid, struct user_regs_struct *r)
{
	struct iovec iov = { .iov_base = r, .iov_len = sizeof(*r) };

	return ptrace(PTRACE_GETREGSET, pid, (void *)NT_PRSTATUS, &iov);
}

static int setregs(int pid, struct user_regs_struct *r)
{
	struct iovec iov = { .iov_base = r, .iov_len = sizeof(*r) };

	return ptrace(PTRACE_SETREGSET, pid, (void *)NT_PRSTATUS, &iov);
}

int main(void)
{
	struct user_regs_struct r, r2;
	unsigned long pc_at_syscall_stop;
	int pid, status, bad = 0;

	pid = fork();
	if (pid == 0) {
		ptrace(PTRACE_TRACEME, 0, 0, 0);
		raise(SIGSTOP);
		__asm__ volatile (
			"ldr x7, =0x1234567890abcdef\n"
			"mov x8, #172\n"	/* getpid: will be emulated away */
			"svc #0\n"
			"mov x1, x7\n"		/* observe x7 after the syscall */
			"mov x8, #93\n"		/* exit(x0) -- tracer sets x0 */
			"mov x0, x1\n"
			"lsr x0, x0, #56\n"
			"svc #0\n"
			::: "x0", "x1", "x7", "x8");
		_exit(0);
	}

	waitpid(pid, &status, 0);
	ptrace(PTRACE_SETOPTIONS, pid, 0, (void *)PTRACE_O_TRACESYSGOOD);

	/* Reach a syscall-entry stop the way UML does: PTRACE_SYSEMU. */
	if (ptrace(PTRACE_SYSEMU, pid, 0, 0)) {
		perror("PTRACE_SYSEMU");
		return 2;
	}
	waitpid(pid, &status, 0);
	if (!WIFSTOPPED(status) || WSTOPSIG(status) != (SIGTRAP | 0x80)) {
		printf("expected a sysemu syscall stop, got status=%x\n", status);
		return 2;
	}
	if (getregs(pid, &r))
		return 2;
	pc_at_syscall_stop = r.pc;
	printf("sysemu entry stop: x7=0x%lx (real value is 0x%lx) pc=0x%lx\n",
	       (unsigned long)r.regs[7], MAGIC, (unsigned long)r.pc);

	/* The escape: single-step out of the syscall stop. */
	if (ptrace(PTRACE_SINGLESTEP, pid, 0, 0)) {
		perror("PTRACE_SINGLESTEP");
		return 2;
	}
	waitpid(pid, &status, 0);
	if (!WIFSTOPPED(status)) {
		printf("tracee did not stop (status=%x)\n", status);
		return 2;
	}
	printf("after PTRACE_SINGLESTEP: sig=%d%s\n", WSTOPSIG(status),
	       WSTOPSIG(status) == (SIGTRAP | 0x80) ? " (a syscall stop -- bad)" :
	       WSTOPSIG(status) == SIGTRAP ? " (SIGTRAP)" : " (unexpected)");
	if (WSTOPSIG(status) != SIGTRAP)
		bad |= 1;

	if (getregs(pid, &r2))
		return 2;
	printf("  x7 = 0x%lx  %s\n", (unsigned long)r2.regs[7],
	       r2.regs[7] == MAGIC ? "== the tracee's real value" : "!! WRONG");
	if (r2.regs[7] != MAGIC)
		bad |= 2;

	printf("  pc = 0x%lx  %s\n", (unsigned long)r2.pc,
	       r2.pc == pc_at_syscall_stop ?
	       "unchanged: no guest instruction was executed" :
	       "MOVED: an instruction ran");
	if (r2.pc != pc_at_syscall_stop)
		bad |= 4;

	/* And is it writable here? */
	r2.regs[7] = NEWX7;
	if (setregs(pid, &r2))
		return 2;
	if (ptrace(PTRACE_SYSEMU, pid, 0, 0))
		return 2;
	waitpid(pid, &status, 0);
	if (!WIFSTOPPED(status) || WSTOPSIG(status) != (SIGTRAP | 0x80)) {
		printf("expected the exit() syscall stop, got status=%x\n", status);
		return 2;
	}
	if (getregs(pid, &r2))
		return 2;
	/* the tracee copied x7 into x1 before this syscall */
	printf("  tracee observed x7 as 0x%lx after resume; wrote 0x%lx  -> %s\n",
	       (unsigned long)r2.regs[1], NEWX7,
	       r2.regs[1] == NEWX7 ? "WRITE TOOK EFFECT" : "write was lost");
	if (r2.regs[1] != NEWX7)
		bad |= 8;

	ptrace(PTRACE_KILL, pid, 0, 0);
	waitpid(pid, &status, 0);

	printf("\nVERDICT: %s (bad=0x%x)\n",
	       bad ? "the escape does NOT work as assumed" :
	       "single-step off a sysemu stop gives a usable x7, costs no guest "
	       "instruction", bad);
	return bad ? 1 : 0;
}
