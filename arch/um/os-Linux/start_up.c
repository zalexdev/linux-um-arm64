// SPDX-License-Identifier: GPL-2.0
/*
 * Copyright (C) 2021 Benjamin Berg <benjamin@sipsolutions.net>
 * Copyright (C) 2000 - 2007 Jeff Dike (jdike@{addtoit,linux.intel}.com)
 */

#include <stdio.h>
#include <stdlib.h>
#include <stdarg.h>
#include <unistd.h>
#include <errno.h>
#include <fcntl.h>
#include <sched.h>
#include <signal.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <sys/time.h>
/* CLOCK_REALTIME for the raw clock_nanosleep() in the seccomp probe below. */
#include <time.h>
#include <sys/resource.h>
#include <asm/unistd.h>
#include <init.h>
#include <os.h>
#include <smp.h>
#include <kern_util.h>
#include <mem_user.h>
#include <ptrace_user.h>
#include <stdbool.h>
#include <stub-data.h>
#include <sys/prctl.h>
#include <linux/seccomp.h>
#include <linux/filter.h>
#include <sysdep/mcontext.h>
#include <sysdep/stub.h>
#include <registers.h>
#include <skas.h>
#include "internal.h"

static void ptrace_child(void)
{
	int ret;
	/* Calling os_getpid because some libcs cached getpid incorrectly */
	int pid = os_getpid(), ppid = getppid();
	int sc_result;

	if (change_sig(SIGWINCH, 0) < 0 ||
	    ptrace(PTRACE_TRACEME, 0, 0, 0) < 0) {
		perror("ptrace");
		kill(pid, SIGKILL);
	}
	kill(pid, SIGSTOP);

	/*
	 * This syscall will be intercepted by the parent. Don't call more than
	 * once, please.
	 */
	sc_result = os_getpid();

	if (sc_result == pid)
		/* Nothing modified by the parent, we are running normally. */
		ret = 1;
	else if (sc_result == ppid)
		/*
		 * Expected in check_ptrace and check_sysemu when they succeed
		 * in modifying the stack frame
		 */
		ret = 0;
	else
		/* Serious trouble! This could be caused by a bug in host 2.6
		 * SKAS3/2.6 patch before release -V6, together with a bug in
		 * the UML code itself.
		 */
		ret = 2;

	exit(ret);
}

static void fatal_perror(const char *str)
{
	perror(str);
	exit(1);
}

static void fatal(char *fmt, ...)
{
	char buf[512];
	va_list list;

	va_start(list, fmt);
	vsnprintf(buf, sizeof(buf), fmt, list);
	va_end(list);
	write(2, buf, strlen(buf));

	exit(1);
}

static void non_fatal(char *fmt, ...)
{
	char buf[512];
	va_list list;

	va_start(list, fmt);
	vsnprintf(buf, sizeof(buf), fmt, list);
	va_end(list);
	write(2, buf, strlen(buf));
}

static int start_ptraced_child(void)
{
	int pid, n, status;

	/* No fflush(stdout): referencing the stdio object pulls in a copy
	 * relocation the UML linker script cannot align. Nothing here uses
	 * buffered stdout. */

	pid = fork();
	if (pid == 0)
		ptrace_child();
	else if (pid < 0)
		fatal_perror("start_ptraced_child : fork failed");

	CATCH_EINTR(n = waitpid(pid, &status, WUNTRACED));
	if (n < 0)
		fatal_perror("check_ptrace : waitpid failed");
	if (!WIFSTOPPED(status) || (WSTOPSIG(status) != SIGSTOP))
		fatal("check_ptrace : expected SIGSTOP, got status = %d",
		      status);

	return pid;
}

static void stop_ptraced_child(int pid, int exitcode)
{
	int status, n;

	if (ptrace(PTRACE_CONT, pid, 0, 0) < 0)
		fatal_perror("stop_ptraced_child : ptrace failed");

	CATCH_EINTR(n = waitpid(pid, &status, 0));
	if (!WIFEXITED(status) || (WEXITSTATUS(status) != exitcode)) {
		int exit_with = WEXITSTATUS(status);
		fatal("stop_ptraced_child : child exited with exitcode %d, "
		      "while expecting %d; status 0x%x\n", exit_with,
		      exitcode, status);
	}
}

/*
 * Reap a probe child without the fatal() that stop_ptraced_child() applies, so
 * a capability that is merely absent can be reported as absent rather than
 * ending the boot.
 */
static int __init finish_probe_child(int pid, int exitcode)
{
	int status, n;

	if (ptrace(PTRACE_CONT, pid, 0, 0) < 0) {
		kill(pid, SIGKILL);
		waitpid(pid, &status, 0);
		return 0;
	}

	CATCH_EINTR(n = waitpid(pid, &status, 0));
	if (n < 0 || !WIFEXITED(status) || WEXITSTATUS(status) != exitcode) {
		kill(pid, SIGKILL);
		waitpid(pid, &status, WNOHANG);
		return 0;
	}
	return 1;
}

/* The classic probe: PTRACE_SYSEMU stops before the syscall and never runs it. */
static int __init try_sysemu(void)
{
	int pid, n, status, count = 0;

	pid = start_ptraced_child();

	if (ptrace(PTRACE_SETOPTIONS, pid, 0, (void *)PTRACE_O_TRACESYSGOOD) < 0)
		fatal_perror("check_sysemu: PTRACE_SETOPTIONS failed");

	while (1) {
		count++;
		if (ptrace(PTRACE_SYSEMU_SINGLESTEP, pid, 0, 0) < 0)
			goto fail;
		CATCH_EINTR(n = waitpid(pid, &status, WUNTRACED));
		if (n < 0)
			fatal_perror("check_sysemu: wait failed");

		if (WIFSTOPPED(status) && WSTOPSIG(status) == (SIGTRAP | 0x80)) {
			if (!count)
				goto fail;
			if (ptrace_set_syscall_ret(pid, os_getpid()) < 0)
				goto fail;
			break;
		} else if (WIFSTOPPED(status) && WSTOPSIG(status) == SIGTRAP) {
			count++;
		} else {
			goto fail;
		}
	}

	return finish_probe_child(pid, 0);

fail:
	kill(pid, SIGKILL);
	CATCH_EINTR(waitpid(pid, &status, 0));
	return 0;
}

/*
 * The substitute, for hosts without PTRACE_SYSEMU.
 *
 * PTRACE_SYSEMU is not doing anything a tracer cannot do for itself: it stops
 * at syscall entry and declines to execute the call. Writing -1 into the
 * in-flight syscall number is the documented way to cancel a syscall from a
 * tracer, and it has been available for as long as the architecture has had a
 * syscall-number regset -- since 3.19 on arm64, where PTRACE_SYSEMU itself only
 * arrived in 5.3. That gap is not academic: phones ship a current Android on a
 * years-old kernel, and 4.19 is a live target.
 *
 * -1 is not always accepted, though. arm64 reports the ptrace stop and *then*
 * runs the seccomp filter, so a filter that screens syscall numbers sees the
 * number the tracer just wrote. Under an Android app sandbox -1 is not on the
 * allowlist and the filter kills the tracee with SIGSYS -- syscall cancellation
 * is unavailable in precisely the environment that also lacks PTRACE_SYSEMU.
 *
 * Substituting a harmless syscall for the guest's works there: the guest's call
 * still does not run, which is the whole requirement, and the substitute is a
 * number the filter already permits. getppid(2) is the natural choice -- no
 * arguments, no side effects, and among the cheapest calls the host has.
 *
 * Probing rather than deriving this from a version number is deliberate. It has
 * to be true of the host actually underneath us -- which may be a container, an
 * emulator, a vendor kernel with its own patches, or a sandbox someone else
 * installed a filter into -- and running the whole sequence end to end is the
 * only thing that establishes that.
 */
enum cancel_ret { RET_LEAVE, RET_PARENT_PID, RET_CHILD_PID };

/*
 * Run, once, exactly what userspace() will do per guest syscall: stop at entry,
 * cancel, step off the stop, then set the return value the guest sees.
 *
 * ptrace_child() exits 1 if it saw its own pid from os_getpid() and 0 if it saw
 * its parent's, so what the child does with the value is what tells the caller
 * whether each half of that took effect. Which of the two is the pass depends
 * on the probe; see try_syscall_cancel().
 */
static int __init run_cancel_probe(int cancel_nr, enum cancel_ret ret_mode,
				   int expect_exit)
{
	int pid, n, status, sig;

	pid = start_ptraced_child();

	if (ptrace(PTRACE_SETOPTIONS, pid, 0, (void *)PTRACE_O_TRACESYSGOOD) < 0)
		fatal_perror("try_syscall_cancel: PTRACE_SETOPTIONS failed");

	if (ptrace(PTRACE_SYSCALL, pid, 0, 0) < 0)
		goto fail;
	CATCH_EINTR(n = waitpid(pid, &status, WUNTRACED));
	if (n < 0)
		fatal_perror("try_syscall_cancel: wait failed");
	if (!WIFSTOPPED(status) || WSTOPSIG(status) != (SIGTRAP | 0x80))
		goto fail;

	if (ptrace_set_syscall_nr(pid, cancel_nr) < 0)
		goto fail;

	if (ptrace(PTRACE_SINGLESTEP, pid, 0, 0) < 0)
		goto fail;
	CATCH_EINTR(n = waitpid(pid, &status, WUNTRACED));
	if (n < 0 || !WIFSTOPPED(status))
		goto fail;

	/*
	 * A seccomp filter that rejected the number written above reports here,
	 * as a SIGSYS the tracer is asked to deliver. Treat any stop that is not
	 * the step as a failure rather than passing the signal on: continuing
	 * would suppress the SIGSYS and let a host that cannot do this look like
	 * one that can.
	 */
	sig = WSTOPSIG(status);
	if (sig != SIGTRAP && sig != (SIGTRAP | 0x80))
		goto fail;

	if (ret_mode != RET_LEAVE &&
	    ptrace_set_syscall_ret(pid, ret_mode == RET_PARENT_PID ?
					os_getpid() : pid) < 0)
		goto fail;

	return finish_probe_child(pid, expect_exit);

fail:
	kill(pid, SIGKILL);
	CATCH_EINTR(waitpid(pid, &status, 0));
	return 0;
}

static int __init try_syscall_cancel(int cancel_nr)
{
	if (cancel_nr < 0)
		/*
		 * A cancelled syscall leaves -ENOSYS in the return register,
		 * which is neither pid nor ppid, so one child covers both
		 * halves: the child can only exit 0 if the call did not run
		 * *and* the write below replaced what it left behind.
		 */
		return run_cancel_probe(cancel_nr, RET_PARENT_PID, 0);

	/*
	 * A substituted getppid() leaves the parent's pid there by itself,
	 * which is the same value a working return-value write would store --
	 * so one child cannot tell the two apart, and each half gets its own.
	 *
	 * First: substitute and write nothing. The child sees its parent's pid
	 * only if getppid() ran in place of its os_getpid(), so exit 0 means
	 * the guest's syscall was really replaced.
	 *
	 * Second: substitute and write the child's own pid, the one value the
	 * substitute cannot produce. Exit 1 means the write took.
	 */
	return run_cancel_probe(cancel_nr, RET_LEAVE, 0) &&
	       run_cancel_probe(cancel_nr, RET_CHILD_PID, 1);
}

static int force_no_sysemu __initdata;
static int force_no_cancel __initdata;

static int __init uml_nosysemu(char *line, int *add)
{
	*add = 0;
	force_no_sysemu = 1;
	return 0;
}

__uml_setup("nosysemu", uml_nosysemu,
"nosysemu\n"
"    Pretend the host has no PTRACE_SYSEMU and use the syscall-cancellation\n"
"    path instead. That path exists for hosts older than the kernel that\n"
"    introduced PTRACE_SYSEMU for the architecture -- 5.3 on arm64 -- which\n"
"    in practice means phones, where a current Android is routinely paired\n"
"    with a years-old kernel.\n"
"\n"
"    Without this option that path can only be exercised by finding such a\n"
"    host, which is precisely the environment that is hardest to debug on.\n"
"    With it, every existing test can run against it on any machine.\n\n"
);

/*
 * Settle on a way to keep a guest syscall from running, and report which.
 * Returns 0 if the host offers none.
 */
static int __init pick_cancel_method(void)
{
	os_info("Checking syscall cancellation instead...");
	if (force_no_cancel) {
		os_info("skipped (nocancel)\n");
	} else if (try_syscall_cancel(-1)) {
		syscall_cancel_nr = -1;
		os_info("OK\n");
		os_info("Host lacks PTRACE_SYSEMU; using PTRACE_SYSCALL with "
			"syscall cancellation (one extra ptrace call per guest "
			"syscall)\n");
		return 1;
	} else {
		os_info("refused\n");
	}

	os_info("Checking syscall substitution instead...");
	if (try_syscall_cancel(__NR_getppid)) {
		syscall_cancel_nr = __NR_getppid;
		os_info("OK\n");
		os_info("Host lacks PTRACE_SYSEMU and rejects a cancelled (-1) "
			"syscall, so a seccomp filter is screening syscall "
			"numbers -- an Android app sandbox does this. Running "
			"getppid(2) in place of each guest syscall instead.\n");
		return 1;
	}
	os_info("missing\n");
	return 0;
}

static int __init uml_nocancel(char *line, int *add)
{
	*add = 0;
	force_no_cancel = 1;
	return 0;
}

__uml_setup("nocancel", uml_nocancel,
"nocancel\n"
"    Pretend the host rejects a cancelled (-1) syscall and substitute a\n"
"    harmless call for each guest syscall instead. Implies nosysemu.\n"
"\n"
"    That is what an Android app sandbox forces, because arm64 runs the\n"
"    seccomp filter after the ptrace stop and the filter then screens the\n"
"    number the tracer wrote. Without this option the substitution path can\n"
"    only be reached on a host that has such a filter installed, which is\n"
"    the environment that is hardest to debug on.\n\n"
);

static void __init check_sysemu(void)
{
	if (force_no_sysemu || force_no_cancel) {
		os_info("Checking syscall emulation for ptrace...");
		os_info("skipped (nosysemu)\n");
		if (pick_cancel_method()) {
			have_ptrace_sysemu = 0;
			return;
		}
		fatal("\nnosysemu was requested but this host can neither "
		      "cancel nor substitute a syscall from a tracer.\n");
	}

	os_info("Checking syscall emulation for ptrace...");
	if (try_sysemu()) {
		have_ptrace_sysemu = 1;
		os_info("OK\n");
		return;
	}
	os_info("missing\n");

	if (pick_cancel_method()) {
		have_ptrace_sysemu = 0;
		return;
	}

	fatal("\nThis host provides neither PTRACE_SYSEMU nor any way for a "
	      "tracer to stop a guest syscall from running, and UML cannot "
	      "intercept guest syscalls without one of them.\n");
}

static void __init check_ptrace(void)
{
	int pid, n, status;
	long syscall;

	os_info("Checking that ptrace can change system call numbers...");
	pid = start_ptraced_child();

	if ((ptrace(PTRACE_SETOPTIONS, pid, 0,
		   (void *) PTRACE_O_TRACESYSGOOD) < 0))
		fatal_perror("check_ptrace: PTRACE_SETOPTIONS failed");

	while (1) {
		if (ptrace(PTRACE_SYSCALL, pid, 0, 0) < 0)
			fatal_perror("check_ptrace : ptrace failed");

		CATCH_EINTR(n = waitpid(pid, &status, WUNTRACED));
		if (n < 0)
			fatal_perror("check_ptrace : wait failed");

		if (!WIFSTOPPED(status) ||
		   (WSTOPSIG(status) != (SIGTRAP | 0x80)))
			fatal("check_ptrace : expected (SIGTRAP|0x80), "
			       "got status = %d", status);

		syscall = ptrace_get_syscall_nr(pid);
		if (syscall == __NR_getpid) {
			n = ptrace_set_syscall_nr(pid, __NR_getppid);
			if (n < 0)
				fatal_perror("check_ptrace : failed to modify "
					     "system call");
			break;
		}
	}
	stop_ptraced_child(pid, 0);
	os_info("OK\n");
	check_sysemu();
}

extern unsigned long host_fp_size;
extern unsigned long exec_regs[MAX_REG_NR];
extern unsigned long *exec_fp_regs;

__initdata static struct stub_data *seccomp_test_stub_data;

/*
 * A stack for the probe helper. Only a few frames deep, but it must be well
 * clear of the shared area -- see the comment at the clone() below.
 */
#define HELPER_STACK_SIZE (64 * 1024)

static void __init sigsys_handler(int sig, siginfo_t *info, void *p)
{
	ucontext_t *uc = p;

	/* Stow away the location of the mcontext in the stack */
	seccomp_test_stub_data->mctx_offset = (unsigned long)&uc->uc_mcontext -
					      (unsigned long)&seccomp_test_stub_data->sigstack[0];

	/* Prevent libc from clearing memory (mctx_offset in particular) */
	syscall(__NR_exit, 0);
}

static int __init seccomp_helper(void *data)
{
	static struct sock_filter filter[] = {
		BPF_STMT(BPF_LD | BPF_W | BPF_ABS,
			 offsetof(struct seccomp_data, nr)),
		BPF_JUMP(BPF_JMP | BPF_JEQ | BPF_K, __NR_clock_nanosleep, 1, 0),
		BPF_STMT(BPF_RET | BPF_K, SECCOMP_RET_ALLOW),
		BPF_STMT(BPF_RET | BPF_K, SECCOMP_RET_TRAP),
	};
	static struct sock_fprog prog = {
		.len = ARRAY_SIZE(filter),
		.filter = filter,
	};
	struct sigaction sa;

	/* close_range is needed for the stub */
	if (stub_syscall3(__NR_close_range, 1, ~0U, 0))
		exit(1);

	/*
	 * Use the whole shared area as the signal stack, exactly as the real
	 * stub does in stub_exe.c, rather than only the sigstack member.
	 *
	 * The member is one page, and one page is not necessarily a legal
	 * alternate stack: the minimum is per-architecture, and arm64's
	 * MINSIGSTKSZ is 5120 against the asm-generic 2048. With 4K pages
	 * sigaltstack() therefore rejects it with ENOMEM and set_sigstack()
	 * panics -- inside a CLONE_VFORK child that has just closed every file
	 * descriptor above zero, so the message goes nowhere and the parent is
	 * left blocked in clone() forever. The visible symptom is UML stopping
	 * dead after "Checking that seccomp filters can be installed...", which
	 * is how SECCOMP mode came to be silently unavailable on arm64.
	 *
	 * The handler still records mctx_offset relative to sigstack[0] and the
	 * frame still lands in the last page, because the stack top is the same
	 * address either way -- this only widens the range that sigaltstack is
	 * told about.
	 */
	set_sigstack(seccomp_test_stub_data,
			sizeof(*seccomp_test_stub_data));

	sa.sa_flags = SA_ONSTACK | SA_NODEFER | SA_SIGINFO;
	sa.sa_sigaction = (void *) sigsys_handler;
	sa.sa_restorer = NULL;
	if (sigaction(SIGSYS, &sa, NULL) < 0)
		exit(2);

	prctl(PR_SET_NO_NEW_PRIVS, 1, 0, 0, 0);
	if (syscall(__NR_seccomp, SECCOMP_SET_MODE_FILTER,
			SECCOMP_FILTER_FLAG_TSYNC, &prog) != 0)
		exit(3);

	/*
	 * Issue the filtered call directly rather than through libc.
	 *
	 * This was sleep(0), and what libc does with that is libc's business:
	 * bionic answers a zero timeout from userspace and never enters the
	 * kernel, so nothing reached the filter and the probe concluded the host
	 * could not trap -- reporting a working seccomp implementation as
	 * missing and dropping UML onto the ptrace path, which costs about four
	 * times as much per guest syscall. glibc happens to make the call, which
	 * is the only reason this ever worked.
	 *
	 * A probe for "does this filter trap" has no business going through a
	 * libc wrapper at all; close_range above is already issued raw for the
	 * same reason.
	 */
	{
		struct timespec ts = { 0, 0 };

		stub_syscall4(__NR_clock_nanosleep, CLOCK_REALTIME, 0,
			      (long) &ts, 0);
	}

	/* Never reached. */
	_exit(4);
}

static bool __init init_seccomp(void)
{
	int pid;
	int status;
	int n;
	unsigned long sp;
	void *helper_stack;

	/*
	 * We check that we can install a seccomp filter and then exit(0)
	 * from a trapped syscall.
	 *
	 * Note that we cannot verify that no seccomp filter already exists
	 * for a syscall that results in the process/thread to be killed.
	 */

	os_info("Checking that seccomp filters can be installed...");

	seccomp_test_stub_data = mmap(0, sizeof(*seccomp_test_stub_data),
				      PROT_READ | PROT_WRITE,
				      MAP_SHARED | MAP_ANON, 0, 0);

	/*
	 * Give the helper a stack of its own rather than carving one out of the
	 * shared area.
	 *
	 * The alternate signal stack registered below covers that whole area,
	 * as it does in the real stub. If the helper's stack were inside it,
	 * sas_ss_flags() would report the thread as already running on the
	 * alternate stack, SA_ONSTACK would be ignored, and the SIGSYS frame
	 * would be pushed onto the helper's own few kilobytes instead -- which
	 * on arm64 is not enough room for a signal frame and kills the helper
	 * with SIGSEGV before the handler is ever entered.
	 */
	helper_stack = mmap(0, HELPER_STACK_SIZE, PROT_READ | PROT_WRITE,
			    MAP_PRIVATE | MAP_ANON, -1, 0);
	if (helper_stack == MAP_FAILED)
		fatal_perror("check_seccomp : stack mmap failed");

	sp = ((unsigned long)helper_stack + HELPER_STACK_SIZE) & ~15UL;
	pid = clone(seccomp_helper, (void *)sp, CLONE_VFORK | CLONE_VM, NULL);

	if (pid < 0)
		fatal_perror("check_seccomp : clone failed");

	CATCH_EINTR(n = waitpid(pid, &status, __WCLONE));
	if (n < 0)
		fatal_perror("check_seccomp : waitpid failed");

	munmap(helper_stack, HELPER_STACK_SIZE);

	if (WIFEXITED(status) && WEXITSTATUS(status) == 0) {
		struct uml_pt_regs *regs;
		unsigned long fp_size;
		int r;

		/*
		 * The handler reported where the host put the signal frame.
		 * Everything downstream indexes sigstack[] with that offset, so
		 * a frame that did not land inside sigstack[] is not merely a
		 * failed probe -- it means this host's signal frames do not fit
		 * the stub's data area, and running SECCOMP mode would have the
		 * stub overwrite its own syscall queue and futex word every time
		 * it takes a signal.
		 *
		 * This is a live concern rather than a theoretical one: an arm64
		 * signal frame is around 4.6 KB against x86-64's ~1 KB, so with
		 * 4 KB pages it does not fit in the single page sigstack[]
		 * currently is. Refuse SECCOMP rather than corrupt the stub;
		 * the ptrace path is unaffected.
		 */
		if (sizeof(seccomp_test_stub_data->sigstack) < sizeof(mcontext_t) ||
		    seccomp_test_stub_data->mctx_offset >
		    sizeof(seccomp_test_stub_data->sigstack) - sizeof(mcontext_t)) {
			os_info("signal frame does not fit the stub data area\n");
			munmap(seccomp_test_stub_data,
			       sizeof(*seccomp_test_stub_data));
			return false;
		}

		/* Fill in the host_fp_size from the mcontext. */
		regs = calloc(1, sizeof(struct uml_pt_regs));
		get_stub_state(regs, seccomp_test_stub_data, &fp_size);
		host_fp_size = fp_size;
		free(regs);

		/* Repeat with the correct size */
		regs = calloc(1, sizeof(struct uml_pt_regs) + host_fp_size);
		r = get_stub_state(regs, seccomp_test_stub_data, NULL);

		/* Store as the default startup registers */
		exec_fp_regs = malloc(host_fp_size);
		memcpy(exec_regs, regs->gp, sizeof(exec_regs));
		memcpy(exec_fp_regs, regs->fp, host_fp_size);

		munmap(seccomp_test_stub_data, sizeof(*seccomp_test_stub_data));

		free(regs);

		if (r) {
			os_info("failed to fetch registers: %d\n", r);
			return false;
		}

		os_info("OK\n");
		return true;
	}

	/*
	 * Say which step failed. The helper runs with every file descriptor
	 * above zero closed, so it cannot report anything itself, and a bare
	 * "error" here is indistinguishable between "this host has no seccomp"
	 * and "UML's own probe is broken" -- which is how a fixable bug in the
	 * probe turned into SECCOMP mode simply never being available.
	 */
	/*
	 * Exit 3 is the one that means "this host does not do seccomp", because
	 * it is SECCOMP_SET_MODE_FILTER that refused. Exit 2 is sigaction()
	 * failing, which says nothing about seccomp and everything about the
	 * probe. Labelling them the other way round -- as this did, following
	 * the single "missing" the code had before -- points the reader at the
	 * host when the bug is here.
	 */
	if (WIFEXITED(status)) {
		switch (WEXITSTATUS(status)) {
		case 1:
			os_info("no close_range\n");
			break;
		case 2:
			os_info("cannot catch SIGSYS\n");
			break;
		case 3:
			os_info("missing\n");
			break;
		case 4:
			os_info("filter did not trap\n");
			break;
		default:
			os_info("helper exited %d\n", WEXITSTATUS(status));
			break;
		}
	} else if (WIFSIGNALED(status)) {
		os_info("helper killed by signal %d\n", WTERMSIG(status));
	} else {
		os_info("error, status 0x%x\n", status);
	}

	munmap(seccomp_test_stub_data, sizeof(*seccomp_test_stub_data));
	return false;
}


static void __init check_coredump_limit(void)
{
	struct rlimit lim;
	int err = getrlimit(RLIMIT_CORE, &lim);

	if (err) {
		perror("Getting core dump limit");
		return;
	}

	os_info("Core dump limits :\n\tsoft - ");
	if (lim.rlim_cur == RLIM_INFINITY)
		os_info("NONE\n");
	else
		os_info("%llu\n", (unsigned long long)lim.rlim_cur);

	os_info("\thard - ");
	if (lim.rlim_max == RLIM_INFINITY)
		os_info("NONE\n");
	else
		os_info("%llu\n", (unsigned long long)lim.rlim_max);
}

/*
 * Hand every line of the host's /proc/cpuinfo to the caller and let it decide
 * what is interesting. The field names are architecture-specific -- x86 wants
 * "flags" and "cache_alignment", arm64 wants "Features" and has no
 * cache_alignment field at all -- so matching them here would bake one
 * architecture's format into generic code. The callback returns non-zero when
 * it has seen everything it needs, which keeps this from reading the whole file
 * on a machine with a lot of cores.
 */
void  __init get_host_cpu_features(int (*line_helper_func)(char *line))
{
	FILE *cpuinfo;
	char *line = NULL;
	size_t len = 0;

	cpuinfo = fopen("/proc/cpuinfo", "r");
	if (cpuinfo == NULL) {
		os_info("Failed to get host CPU features\n");
	} else {
		while ((getline(&line, &len, cpuinfo)) != -1) {
			int done = line_helper_func(line);

			free(line);
			line = NULL;
			if (done)
				break;
		}
		fclose(cpuinfo);
	}
}

static int seccomp_config __initdata;

static int __init uml_seccomp_config(char *line, int *add)
{
	*add = 0;

	if (strcmp(line, "off") == 0)
		seccomp_config = 0;
	else if (strcmp(line, "auto") == 0)
		seccomp_config = 1;
	else if (strcmp(line, "on") == 0)
		seccomp_config = 2;
	else
		fatal("Invalid seccomp option '%s', expected on/auto/off\n",
		      line);

	return 0;
}

__uml_setup("seccomp=", uml_seccomp_config,
"seccomp=<on/auto/off>\n"
"    Configure whether or not SECCOMP is used. With SECCOMP, userspace\n"
"    processes work collaboratively with the kernel instead of being\n"
"    traced using ptrace. All syscalls from the application are caught and\n"
"    redirected using a signal. This signal handler in turn is permitted to\n"
"    do the selected set of syscalls to communicate with the UML kernel and\n"
"    do the required memory management.\n"
"\n"
"    This method is overall faster than the ptrace based userspace, primarily\n"
"    because it reduces the number of context switches for (minor) page faults.\n"
"\n"
"    However, the SECCOMP filter is not (yet) restrictive enough to prevent\n"
"    userspace from reading and writing all physical memory. Userspace\n"
"    processes could also trick the stub into disabling SIGALRM which\n"
"    prevents it from being interrupted for scheduling purposes.\n"
"\n"
"    This is insecure and should only be used with a trusted userspace\n\n"
);

extern long elf_aux_min_sigstack;

/*
 * Report, and sanity-check, the size of the alternate signal stack the stub
 * gets against what this host says a signal frame can need.
 *
 * The stub takes its signals on an alternate stack that is part of struct
 * stub_data, whose size is fixed at compile time. The frame written there is
 * built by the host from the host's CPU features and from what the guest has
 * executed inside the stub -- neither of which UML chooses. On arm64 the same
 * kernel reports AT_MINSIGSTKSZ of 4720 on one CPU model and 9984 on another,
 * so a compile-time size taken from a measurement on one machine is not a
 * design, it is a coincidence waiting to end.
 *
 * This does not refuse to boot when the area is smaller than AT_MINSIGSTKSZ,
 * because that number is the host's worst case (it is computed with every
 * optional record present at the maximum vector length) and the frames actually
 * written are usually far smaller -- 4576 bytes on both hosts measured. A frame
 * that genuinely does not fit fails loudly on its own: the host cannot write it,
 * the stub dies, and UML reports that. What is worth avoiding is being
 * surprised, so say the numbers out loud at boot instead.
 *
 * SECCOMP mode is stricter and is handled separately: there the frame has to
 * land inside sigstack[] alone, and init_seccomp() checks that it did.
 */
static void __init check_stub_sigstack(void)
{
	/*
	 * The stack itself, not the whole stub data area: one of those pages is
	 * bookkeeping and no signal frame may land in it. Reporting the larger
	 * number would overstate the headroom by a page and could describe a
	 * host as fitting when it does not.
	 */
	unsigned long have = sizeof(((struct stub_data *)NULL)->sigstack);

	if (!elf_aux_min_sigstack) {
		os_info("Stub signal stack: %lu bytes (host publishes no AT_MINSIGSTKSZ)\n",
			have);
		return;
	}

	os_info("Stub signal stack: %lu bytes, host AT_MINSIGSTKSZ %ld%s\n",
		have, elf_aux_min_sigstack,
		have < (unsigned long)elf_aux_min_sigstack ?
			" -- SMALLER than this host's worst case" : "");
}

/*
 * A guest page is a host mmap(), so the guest's page size cannot be smaller
 * than the host's: mmap() rejects a length or offset that is not a multiple of
 * the host's granularity, and UML has nothing to fall back on.
 *
 * Getting this wrong is not a subtle failure, but it is a silent one. A UML
 * built with 4 KB pages on a 16 KB host stops dead just after the ptrace
 * capability checks, with no message, because the first thing it does
 * afterwards is map the stub. That is a live configuration and not a corner
 * case: Android 15 and later ship 16 KB kernels, arm64 servers ship 4 KB, and
 * the default here is 4 KB -- so the first person to run this on a modern phone
 * gets a hang and no clue.
 *
 * The other direction is fine and needs no check: a 16 KB guest on a 4 KB host
 * simply uses four host pages per guest page.
 */
static void __init check_page_size(void)
{
	unsigned long host = getpagesize();

	if (host <= UM_KERN_PAGE_SIZE)
		return;

	fatal("Host page size is %lu bytes but this kernel was built for %lu.\n"
	      "A guest page is a host mmap(), so it cannot be smaller than the\n"
	      "host's page size. Rebuild with CONFIG_PAGE_SIZE_%luKB.\n",
	      host, (unsigned long)UM_KERN_PAGE_SIZE, host / 1024);
}

void __init os_early_checks(void)
{
	int pid;

	/* Nothing below can work if this is wrong, so check it before anything */
	check_page_size();

	/* Print out the core dump limits early */
	check_coredump_limit();

	check_stub_sigstack();

	/* Need to check this early because mmapping happens before the
	 * kernel is running.
	 */
	check_tmpexec();

	if (seccomp_config) {
		if (init_seccomp()) {
			using_seccomp = 1;
			/*
			 * Which mode is in use decides how much every guest
			 * syscall costs -- SECCOMP handles them in-process via
			 * SIGSYS, ptrace pays a stop and a waitpid per call --
			 * so it is the first thing anyone asks when the guest
			 * feels slow. Saying it once at boot is cheaper than
			 * inferring it from which probe lines are absent.
			 */
			os_info("Userspace mode: SECCOMP\n");
			return;
		}

		if (seccomp_config == 2)
			fatal("SECCOMP userspace requested but not functional!\n");
	}

	if (uml_ncpus > 1)
		fatal("SMP is not supported with PTRACE userspace.\n");

	using_seccomp = 0;
	os_info("Userspace mode: ptrace\n");
	check_ptrace();

	pid = start_ptraced_child();
	if (init_pid_registers(pid))
		fatal("Failed to initialize default registers");
	stop_ptraced_child(pid, 1);
}
