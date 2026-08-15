// SPDX-License-Identifier: GPL-2.0
/*
 * Gate 11: ptrace *inside* the guest.
 *
 * Gates 1-9 never exercise this. They run shells, package managers and
 * compilers, none of which trace anything -- so the guest's own ptrace
 * implementation, arch/arm64/um/ptrace.c and the regset plumbing behind it, has
 * never been executed by any test in this project.
 *
 * That is the wrong place to have a blind spot. It is the same register
 * interface in which two bugs were just found by audit rather than by failure
 * (rt_sigreturn clearing ORIG_SYSCALL instead of SYSCALL_NR, and -ENOSYS being
 * seeded into x0 before the entry stop), and it is a difficult interface on
 * arm64 specifically, because the syscall number is not in the register set --
 * it lives in its own NT_ARM_SYSTEM_CALL regset -- and because the return value
 * and the first argument are the same register.
 *
 * It also matters in its own right: gdb and strace are how the next phase
 * debugs guest drivers, and both are built entirely out of what is checked
 * here.
 *
 * Each check prints PASS or FAIL with the values, so a failure says what the
 * tracer saw rather than merely that something is wrong.
 *
 * Build (runs in the guest):
 *   clang --target=aarch64-linux-gnu -static -O2 -o guestptrace guestptrace.c
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

#define __NR_getpid_    172
#define __NR_write_     64

static int failures;

static void check(int ok, const char *name, const char *fmt, ...)
{
	va_list ap;

	printf("%-4s %-34s ", ok ? "PASS" : "FAIL", name);
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

static long get_scno(int pid)
{
	int scno = -2;
	struct iovec iov = { .iov_base = &scno, .iov_len = sizeof(scno) };

	if (ptrace(PTRACE_GETREGSET, pid, (void *)NT_ARM_SYSTEM_CALL, &iov))
		return -99;
	return scno;
}

/*
 * The tracee. Issues syscalls whose arguments are known constants, so a tracer
 * that reports something else is demonstrably wrong rather than merely
 * surprising.
 */
static void tracee(void)
{
	ptrace(PTRACE_TRACEME, 0, 0, 0);
	raise(SIGSTOP);

	/* write(0x1234, 0x5678, 0x9abc) -- deliberately invalid, only the
	 * argument values matter, and EBADF is a stable result. */
	__asm__ volatile (
		"mov x0, #0x1234\n"
		"mov x1, #0x5678\n"
		"mov x2, #0x9abc\n"
		"mov x8, %0\n"
		"svc #0\n"
		:: "i" (__NR_write_)
		: "x0", "x1", "x2", "x8", "memory");

	__asm__ volatile (
		"mov x8, %0\n"
		"svc #0\n"
		:: "i" (__NR_getpid_) : "x0", "x8", "memory");

	_exit(0);
}

int main(void)
{
	struct user_regs_struct r, r2;
	unsigned long pc_entry;
	int pid, status;
	long scno;

	setvbuf(stdout, NULL, _IONBF, 0);
	printf("GUESTPTRACE_START\n");

	pid = fork();
	if (pid == 0) {
		tracee();
		_exit(1);
	}

	waitpid(pid, &status, 0);
	check(WIFSTOPPED(status) && WSTOPSIG(status) == SIGSTOP,
	      "initial stop", "status=0x%x", status);
	if (ptrace(PTRACE_SETOPTIONS, pid, 0, (void *)PTRACE_O_TRACESYSGOOD))
		check(0, "PTRACE_O_TRACESYSGOOD", "errno=%d", errno);
	else
		check(1, "PTRACE_O_TRACESYSGOOD", "ok");

	/* --- syscall-entry stop for write(0x1234, 0x5678, 0x9abc) --- */
	if (ptrace(PTRACE_SYSCALL, pid, 0, 0)) {
		check(0, "PTRACE_SYSCALL", "errno=%d", errno);
		goto out;
	}
	waitpid(pid, &status, 0);
	check(WIFSTOPPED(status) && WSTOPSIG(status) == (SIGTRAP | 0x80),
	      "syscall-entry stop reported", "status=0x%x (want 0x85..)", status);

	if (getregs(pid, &r)) {
		check(0, "GETREGSET NT_PRSTATUS", "errno=%d", errno);
		goto out;
	}
	check(1, "GETREGSET NT_PRSTATUS", "ok");
	pc_entry = r.pc;

	scno = get_scno(pid);
	check(scno == __NR_write_, "NT_ARM_SYSTEM_CALL at entry",
	      "got %ld, want %d", scno, __NR_write_);

	check(r.regs[8] == __NR_write_, "x8 at entry",
	      "got 0x%lx, want 0x%x", (unsigned long)r.regs[8], __NR_write_);

	/*
	 * The one the audit predicted would fail: on native arm64 x0 at a
	 * syscall-entry stop is the first argument. If UML has seeded the
	 * return register with -ENOSYS before the stop, a tracer sees
	 * 0xffffffffffffffda instead, and every argument-inspecting tool --
	 * strace without PTRACE_GET_SYSCALL_INFO, ltrace, seccomp-style
	 * sandboxes -- reads the wrong value for every syscall.
	 */
	check(r.regs[0] == 0x1234, "arg1 (x0) at entry",
	      "got 0x%lx, want 0x1234%s", (unsigned long)r.regs[0],
	      r.regs[0] == (unsigned long)-38 ? "  [-ENOSYS: seeded before the stop]" : "");
	check(r.regs[1] == 0x5678, "arg2 (x1) at entry",
	      "got 0x%lx, want 0x5678", (unsigned long)r.regs[1]);
	check(r.regs[2] == 0x9abc, "arg3 (x2) at entry",
	      "got 0x%lx, want 0x9abc", (unsigned long)r.regs[2]);

	/* --- syscall-exit stop --- */
	if (ptrace(PTRACE_SYSCALL, pid, 0, 0))
		goto out;
	waitpid(pid, &status, 0);
	check(WIFSTOPPED(status) && WSTOPSIG(status) == (SIGTRAP | 0x80),
	      "syscall-exit stop reported", "status=0x%x", status);

	if (getregs(pid, &r2))
		goto out;
	check((long)r2.regs[0] == -EBADF, "return value at exit",
	      "got %ld, want %d (-EBADF)", (long)r2.regs[0], -EBADF);
	check(r2.pc == pc_entry, "pc unchanged across the syscall",
	      "entry 0x%lx exit 0x%lx", pc_entry, (unsigned long)r2.pc);

	/* --- writing a register at a stop must take effect --- */
	if (ptrace(PTRACE_SYSCALL, pid, 0, 0))
		goto out;
	waitpid(pid, &status, 0);	/* entry stop for getpid */
	scno = get_scno(pid);
	check(scno == __NR_getpid_, "NT_ARM_SYSTEM_CALL, 2nd syscall",
	      "got %ld, want %d", scno, __NR_getpid_);

	if (getregs(pid, &r))
		goto out;
	r.regs[9] = 0xfeedfacecafebeefUL;	/* x9: caller-saved, unused here */
	if (setregs(pid, &r))
		check(0, "SETREGSET NT_PRSTATUS", "errno=%d", errno);
	else
		check(1, "SETREGSET NT_PRSTATUS", "ok");
	if (getregs(pid, &r2))
		goto out;
	check(r2.regs[9] == 0xfeedfacecafebeefUL, "register write reads back",
	      "got 0x%lx", (unsigned long)r2.regs[9]);

	/* --- single-step --- */
	if (ptrace(PTRACE_SYSCALL, pid, 0, 0))
		goto out;
	waitpid(pid, &status, 0);	/* exit stop for getpid */

	if (ptrace(PTRACE_SINGLESTEP, pid, 0, 0)) {
		check(0, "PTRACE_SINGLESTEP accepted", "errno=%d", errno);
	} else {
		waitpid(pid, &status, 0);
		check(WIFSTOPPED(status) && WSTOPSIG(status) == SIGTRAP,
		      "PTRACE_SINGLESTEP -> SIGTRAP", "status=0x%x", status);
		if (!getregs(pid, &r2))
			check(r2.pc != pc_entry, "pc advanced by the step",
			      "pc=0x%lx", (unsigned long)r2.pc);
	}

	/* --- PEEKDATA on the tracee's text --- */
	errno = 0;
	{
		long word = ptrace(PTRACE_PEEKDATA, pid, (void *)r2.pc, 0);

		check(!(word == -1 && errno), "PTRACE_PEEKDATA", "word=0x%lx errno=%d",
		      (unsigned long)word, errno);
	}

out:
	ptrace(PTRACE_KILL, pid, 0, 0);
	kill(pid, SIGKILL);
	waitpid(pid, &status, 0);

	printf("\nGUESTPTRACE failures=%d\n", failures);
	printf(failures ? "GUESTPTRACE_FAILED\n" : "GUESTPTRACE_OK\n");
	return failures ? 1 : 0;
}
