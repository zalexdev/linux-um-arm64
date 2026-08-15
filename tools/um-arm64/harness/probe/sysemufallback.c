// SPDX-License-Identifier: GPL-2.0
/*
 * Can UML drive a stub on a host kernel that has no PTRACE_SYSEMU?
 *
 * arm64 gained PTRACE_SYSEMU in v5.3. Android devices that are current at the
 * application level are routinely not current at the kernel level -- the phone
 * this was written for runs Android 15 on a 4.19 kernel -- so "the host is
 * newer than 5.3" is an assumption that fails on real targets rather than a
 * safe floor.
 *
 * PTRACE_SYSEMU is not magic: it stops at syscall entry and does not execute
 * the syscall. The same effect is reachable on any arm64 kernel old enough to
 * have the NT_ARM_SYSTEM_CALL regset, by stopping with PTRACE_SYSCALL and
 * writing -1 into the syscall number, which is the documented way to cancel a
 * syscall from a tracer. The cost is two stops per syscall instead of one.
 *
 * This measures whether that substitution actually holds on this host, which
 * is four separate questions:
 *
 *   1. is PTRACE_SYSEMU really absent (and with which errno)?
 *   2. does writing -1 to NT_ARM_SYSTEM_CALL cancel the syscall?
 *   3. can the tracer then set the return value the guest sees?
 *   4. does PTRACE_SINGLESTEP off an entry stop still work? -- the port's x7
 *      recovery depends on it, and it is a separate kernel path from SYSEMU.
 *
 * Build:
 *   clang --target=aarch64-linux-gnu -static -O1 -Wall -o sysemufallback sysemufallback.c
 */
#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>
#include <stdarg.h>
#include <signal.h>
#include <sys/ptrace.h>
#include <sys/wait.h>
#include <sys/uio.h>
#include <sys/user.h>
#include <linux/elf.h>

#ifndef NT_ARM_SYSTEM_CALL
#define NT_ARM_SYSTEM_CALL 0x404
#endif
#ifndef PTRACE_SYSEMU
#define PTRACE_SYSEMU 31
#endif
#ifndef PTRACE_SYSEMU_SINGLESTEP
#define PTRACE_SYSEMU_SINGLESTEP 32
#endif

#define __NR_getpid_ 172

static int failures;

static void check(int ok, const char *name, const char *fmt, ...)
{
	va_list ap;

	printf("%-4s %-36s ", ok ? "PASS" : "FAIL", name);
	va_start(ap, fmt);
	vprintf(fmt, ap);
	va_end(ap);
	putchar('\n');
	if (!ok)
		failures++;
}

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

static int set_scno(int pid, int scno)
{
	struct iovec iov = { .iov_base = &scno, .iov_len = sizeof(scno) };

	return ptrace(PTRACE_SETREGSET, pid, (void *)NT_ARM_SYSTEM_CALL, &iov);
}

/*
 * The tracee calls getpid() and reports what it got back. If the cancel works,
 * the value it sees is whatever the tracer wrote, not its real pid -- which is
 * exactly the substitution UML needs, since UML answers guest syscalls itself.
 */
static void tracee(void)
{
	long ret;

	ptrace(PTRACE_TRACEME, 0, 0, 0);
	raise(SIGSTOP);

	__asm__ volatile (
		"mov x8, %1\n"
		"svc #0\n"
		"mov %0, x0\n"
		: "=r" (ret) : "i" (__NR_getpid_) : "x0", "x8", "memory");

	_exit((int)(ret & 0x7f));
}

int main(void)
{
	struct user_regs_struct r;
	const int magic = 0x2a;		/* fits in an exit status */
	int pid, status;

	setvbuf(stdout, NULL, _IONBF, 0);
	printf("SYSEMUFB_START\n");
	printf("real_pid=%d\n", (int)getpid());

	/* --- 1. is PTRACE_SYSEMU present at all? --- */
	pid = fork();
	if (pid == 0) { tracee(); _exit(1); }
	waitpid(pid, &status, 0);

	errno = 0;
	if (ptrace(PTRACE_SYSEMU_SINGLESTEP, pid, 0, 0) < 0) {
		printf("     %-36s absent, errno=%d (%s)\n",
		       "PTRACE_SYSEMU_SINGLESTEP", errno, strerror(errno));
	} else {
		printf("     %-36s PRESENT -- no fallback needed\n",
		       "PTRACE_SYSEMU_SINGLESTEP");
		waitpid(pid, &status, 0);
	}
	kill(pid, SIGKILL);
	waitpid(pid, &status, 0);

	/* --- 2/3. cancel via NT_ARM_SYSTEM_CALL, then set the return value --- */
	pid = fork();
	if (pid == 0) { tracee(); _exit(1); }
	waitpid(pid, &status, 0);
	ptrace(PTRACE_SETOPTIONS, pid, 0, (void *)PTRACE_O_TRACESYSGOOD);

	if (ptrace(PTRACE_SYSCALL, pid, 0, 0)) {
		check(0, "PTRACE_SYSCALL to entry", "errno=%d", errno);
		goto out;
	}
	waitpid(pid, &status, 0);
	check(WIFSTOPPED(status) && WSTOPSIG(status) == (SIGTRAP | 0x80),
	      "entry stop", "status=0x%x", status);

	if (set_scno(pid, -1)) {
		check(0, "write -1 to NT_ARM_SYSTEM_CALL", "errno=%d (%s)",
		      errno, strerror(errno));
		goto out;
	}
	check(1, "write -1 to NT_ARM_SYSTEM_CALL", "ok");

	/*
	 * PTRACE_SINGLESTEP off the entry stop, not PTRACE_SYSCALL: the port's
	 * x7 recovery already does exactly this, so if it works the fallback
	 * and the existing workaround are the same mechanism.
	 */
	if (ptrace(PTRACE_SINGLESTEP, pid, 0, 0)) {
		check(0, "SINGLESTEP off entry stop", "errno=%d", errno);
		goto out;
	}
	waitpid(pid, &status, 0);
	check(WIFSTOPPED(status), "stop after singlestep", "status=0x%x", status);

	if (getregs(pid, &r)) {
		check(0, "GETREGSET after cancel", "errno=%d", errno);
		goto out;
	}
	r.regs[0] = magic;
	if (setregs(pid, &r)) {
		check(0, "set return value", "errno=%d", errno);
		goto out;
	}
	check(1, "set return value", "x0 <- 0x%x", magic);

	ptrace(PTRACE_CONT, pid, 0, 0);
	waitpid(pid, &status, 0);

	/*
	 * The tracee exits with what getpid() returned. magic means the syscall
	 * never ran and the tracer's value was seen instead -- i.e. SYSEMU
	 * semantics without SYSEMU. Its real pid means the syscall executed and
	 * the cancel did not take.
	 */
	if (WIFEXITED(status)) {
		int got = WEXITSTATUS(status);

		check(got == magic, "syscall cancelled, tracer value seen",
		      "tracee saw %d, want %d%s", got, magic,
		      got == (pid & 0x7f) ? "  [real pid: syscall RAN]" : "");
	} else {
		check(0, "tracee exited", "status=0x%x", status);
	}

out:
	kill(pid, SIGKILL);
	waitpid(pid, &status, 0);

	printf("\nSYSEMUFB failures=%d\n", failures);
	printf(failures ? "SYSEMUFB_FAILED\n" : "SYSEMUFB_OK\n");
	return failures ? 1 : 0;
}
