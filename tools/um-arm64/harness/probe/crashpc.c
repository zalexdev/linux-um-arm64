// SPDX-License-Identifier: GPL-2.0
/*
 * Report where a program crashed, when nothing else will tell you.
 *
 * A statically linked binary that dies before libc's startup finishes gets no
 * tombstone: Android's debuggerd handler is installed by __libc_init, so a
 * crash before that point is reported as nothing more than "Segmentation
 * fault" from the shell. Remote debugging over wifi adb was not connecting
 * either, and the whole question here is a single number -- the faulting PC.
 *
 * So: trace the child ourselves. ptrace works for the shell uid on this device
 * (measured, see guestptrace.c), which makes this about sixty lines and
 * completely reliable.
 *
 * Prints the fault address, the PC, the instruction at the PC, and enough
 * registers to tell a null dereference from a jump into unmapped memory.
 *
 * Build:
 *   clang --target=aarch64-linux-gnu -static -O1 -o crashpc crashpc.c
 * Use:
 *   ./crashpc ./linux-bionic --version
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
#include <sys/user.h>
#include <linux/elf.h>

int main(int argc, char **argv)
{
	struct user_regs_struct r;
	struct iovec iov = { .iov_base = &r, .iov_len = sizeof(r) };
	siginfo_t si;
	int status;
	pid_t pid;

	if (argc < 2) {
		fprintf(stderr, "usage: crashpc <program> [args...]\n");
		return 2;
	}

	pid = fork();
	if (pid == 0) {
		ptrace(PTRACE_TRACEME, 0, 0, 0);
		execv(argv[1], &argv[1]);
		fprintf(stderr, "exec %s failed: %d\n", argv[1], errno);
		_exit(127);
	}

	setvbuf(stdout, NULL, _IONBF, 0);

	for (;;) {
		if (waitpid(pid, &status, 0) < 0) {
			perror("waitpid");
			return 1;
		}

		if (WIFEXITED(status)) {
			printf("exited normally, status %d\n", WEXITSTATUS(status));
			return 0;
		}
		if (!WIFSTOPPED(status)) {
			printf("terminated by signal %d\n", WTERMSIG(status));
			return 0;
		}

		int sig = WSTOPSIG(status);

		/* The first stop is the exec trap; let it run on. */
		if (sig == SIGTRAP) {
			ptrace(PTRACE_CONT, pid, 0, 0);
			continue;
		}

		if (sig != SIGSEGV && sig != SIGBUS && sig != SIGILL) {
			/* Not ours -- pass it through. */
			ptrace(PTRACE_CONT, pid, 0, sig);
			continue;
		}

		printf("\n=== %s ===\n", strsignal(sig));

		if (ptrace(PTRACE_GETSIGINFO, pid, 0, &si) == 0)
			printf("fault addr : %p  (si_code %d)\n",
			       si.si_addr, si.si_code);

		if (ptrace(PTRACE_GETREGSET, pid, (void *)NT_PRSTATUS, &iov) == 0) {
			unsigned long insn = 0;

			printf("pc         : 0x%llx\n", (unsigned long long)r.pc);
			printf("lr         : 0x%llx\n", (unsigned long long)r.regs[30]);
			printf("sp         : 0x%llx\n", (unsigned long long)r.sp);
			printf("x0..x3     : %llx %llx %llx %llx\n",
			       (unsigned long long)r.regs[0], (unsigned long long)r.regs[1],
			       (unsigned long long)r.regs[2], (unsigned long long)r.regs[3]);

			errno = 0;
			insn = ptrace(PTRACE_PEEKTEXT, pid, (void *)r.pc, 0);
			if (!errno)
				printf("insn at pc : 0x%08x\n", (unsigned int)insn);
			else
				printf("insn at pc : unreadable (pc is not mapped)\n");
		}

		kill(pid, SIGKILL);
		waitpid(pid, &status, 0);
		return 0;
	}
}
