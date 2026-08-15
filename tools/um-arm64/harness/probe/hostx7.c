// SPDX-License-Identifier: GPL-2.0
/*
 * Host fact: x7 is not readable or writable at an arm64 ptrace syscall stop.
 *
 * arm64's ptrace encodes "is this stop a syscall entry or a syscall exit" by
 * overwriting a general-purpose register in the tracee -- x7 on AArch64, r12 on
 * AArch32 -- and restoring the tracee's own value once the stop ends. The
 * kernel says so itself, in ptrace_save_reg():
 *
 *	- Any writes by the tracer to this register during the stop are
 *	  ignored/discarded.
 *	- The actual value of the register is not available during the stop,
 *	  so the tracer cannot save it and restore it later.
 *	- Syscall stops behave differently to seccomp and pseudo-step traps
 *	  (the latter do not nobble any registers).
 *
 * That is a direct problem for UML, whose whole design is "read the guest's
 * registers at a stop, keep them, write them back later" -- and which multiplexes
 * every thread of one guest mm onto a single traced stub process. This program
 * establishes the behaviour against the actual host rather than against the
 * source, and checks the pseudo-step claim too, because the fix depends on it.
 *
 * Prints one line per fact, and PASS/FAIL for whether the port's assumption
 * ("a register written at a stop takes effect") holds.
 *
 * Build and run on the host that ./linux will run on:
 *   cc -O2 -o hostx7 hostx7.c && ./hostx7
 */
#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>
#include <sys/ptrace.h>
#include <sys/wait.h>
#include <sys/uio.h>
#include <linux/elf.h>
#include <linux/ptrace.h>

#ifndef PTRACE_SYSEMU
#define PTRACE_SYSEMU 31
#endif

#define MAGIC	0x1234567890abcdefUL

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
	struct user_regs_struct r;
	int pid, status, fail = 0;

	pid = fork();
	if (pid == 0) {
		ptrace(PTRACE_TRACEME, 0, 0, 0);
		raise(SIGSTOP);
		/*
		 * Put a known value in x7 and issue two syscalls. Between them
		 * the tracer will try to change x7; the second syscall's stop is
		 * where we look at what actually happened.
		 */
		__asm__ volatile (
			"ldr x7, =0x1234567890abcdef\n"
			"mov x8, #172\n"	/* getpid */
			"svc #0\n"
			"mov x8, #172\n"
			"svc #0\n"
			"mov x0, x7\n"		/* hand x7 back as the exit code */
			"lsr x0, x0, #56\n"
			"mov x8, #93\n"		/* exit */
			"svc #0\n"
			::: "x0", "x7", "x8");
		_exit(0);
	}

	waitpid(pid, &status, 0);
	ptrace(PTRACE_SETOPTIONS, pid, 0, (void *)PTRACE_O_TRACESYSGOOD);

	/* Run to the first syscall-entry stop. */
	ptrace(PTRACE_SYSCALL, pid, 0, 0);
	waitpid(pid, &status, 0);
	if (!WIFSTOPPED(status) || WSTOPSIG(status) != (SIGTRAP | 0x80)) {
		printf("no syscall stop (status=%x)\n", status);
		return 2;
	}

	if (getregs(pid, &r)) {
		perror("GETREGSET");
		return 2;
	}
	printf("at syscall-entry stop: x7 reads as 0x%lx (tracee set 0x%lx)\n",
	       (unsigned long)r.regs[7], MAGIC);
	if (r.regs[7] == MAGIC) {
		printf("  -> x7 IS readable at a syscall stop\n");
	} else {
		printf("  -> x7 is NOT readable: the value is the stop direction "
		       "(%lu = %s)\n", (unsigned long)r.regs[7],
		       r.regs[7] ? "EXIT" : "ENTER");
		fail |= 1;
	}

	/* Try to change it. */
	r.regs[7] = 0xdeadbeefUL;
	if (setregs(pid, &r)) {
		perror("SETREGSET");
		return 2;
	}
	if (getregs(pid, &r))
		return 2;
	printf("after writing 0xdeadbeef, readback at the same stop: 0x%lx\n",
	       (unsigned long)r.regs[7]);

	/* Let the syscall run to its exit stop, then to the next entry stop. */
	ptrace(PTRACE_SYSCALL, pid, 0, 0);
	waitpid(pid, &status, 0);
	ptrace(PTRACE_SYSCALL, pid, 0, 0);
	waitpid(pid, &status, 0);
	if (getregs(pid, &r))
		return 2;
	printf("at the next syscall stop: x7 = 0x%lx\n",
	       (unsigned long)r.regs[7]);

	/*
	 * Now the pseudo-step trap, which the kernel comment says does not
	 * nobble anything. If that holds, it is a stop at which the register
	 * can be both read and written -- which is what a fix has to use.
	 */
	ptrace(PTRACE_SINGLESTEP, pid, 0, 0);
	waitpid(pid, &status, 0);
	if (WIFSTOPPED(status) && WSTOPSIG(status) == SIGTRAP) {
		if (getregs(pid, &r))
			return 2;
		printf("at a single-step SIGTRAP stop: x7 = 0x%lx%s\n",
		       (unsigned long)r.regs[7],
		       r.regs[7] == MAGIC ? "  (the tracee's real value)" : "");
		r.regs[7] = 0x5555UL;
		setregs(pid, &r);
		if (getregs(pid, &r))
			return 2;
		printf("  write at a step stop reads back as 0x%lx -> %s\n",
		       (unsigned long)r.regs[7],
		       r.regs[7] == 0x5555UL ? "WRITABLE" : "discarded");
		if (r.regs[7] != 0x5555UL)
			fail |= 2;
	} else {
		printf("no single-step stop (status=%x)\n", status);
		fail |= 2;
	}

	ptrace(PTRACE_KILL, pid, 0, 0);
	waitpid(pid, &status, 0);

	printf("\nVERDICT: syscall stop hides x7: %s; step stop exposes it: %s\n",
	       (fail & 1) ? "YES" : "no",
	       (fail & 2) ? "no" : "YES");
	return 0;
}
