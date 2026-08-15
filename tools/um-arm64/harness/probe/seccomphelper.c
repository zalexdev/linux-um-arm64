// SPDX-License-Identifier: GPL-2.0
/*
 * Standalone replica of UML's SECCOMP capability probe.
 *
 * init_seccomp() in arch/um/os-Linux/start_up.c clones a helper that installs a
 * seccomp filter trapping one syscall, triggers it, and exits from the SIGSYS
 * handler. On arm64 that helper dies with SIGSEGV, and it cannot say why: it
 * runs after close_range(1, ~0U), so it has no descriptor to complain on, and
 * it shares its parent's memory under CLONE_VM|CLONE_VFORK, so the parent is
 * blocked in clone() the whole time.
 *
 * This is the same sequence with the diagnostics put back. Descriptor 0 is
 * pointed at stderr before cloning, because close_range(1, ~0U) leaves it
 * alone, so each step can report itself. Every step is reported before it is
 * attempted, so the last line printed names the step that killed the child.
 *
 * Build and run on the host ./linux runs on:
 *   cc -O2 -o seccomphelper seccomphelper.c && ./seccomphelper
 */
#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>
#include <stddef.h>
#include <sched.h>
#include <signal.h>
#include <sys/mman.h>
#include <sys/prctl.h>
#include <sys/syscall.h>
#include <sys/wait.h>
#include <linux/filter.h>
#include <linux/seccomp.h>
#include <ucontext.h>

#define AREA	8192

static void *area;
static void *altsp;
static unsigned long altsz;
static volatile unsigned long mctx_offset;

static void say(const char *s)
{
	/* fd 0, the one close_range(1, ~0U) leaves behind */
	write(0, s, strlen(s));
}

static void sigsys_handler(int sig, siginfo_t *info, void *p)
{
	ucontext_t *uc = p;

	say("  child: SIGSYS handler entered\n");
	mctx_offset = (unsigned long)&uc->uc_mcontext - (unsigned long)area;
	say("  child: recorded mctx_offset, exiting 0\n");
	syscall(__NR_exit, 0);
}

static int helper(void *data)
{
	static struct sock_filter filter[] = {
		BPF_STMT(BPF_LD | BPF_W | BPF_ABS,
			 offsetof(struct seccomp_data, nr)),
		BPF_JUMP(BPF_JMP | BPF_JEQ | BPF_K, __NR_clock_nanosleep, 1, 0),
		BPF_STMT(BPF_RET | BPF_K, SECCOMP_RET_ALLOW),
		BPF_STMT(BPF_RET | BPF_K, SECCOMP_RET_TRAP),
	};
	static struct sock_fprog prog = {
		.len = sizeof(filter) / sizeof(filter[0]),
		.filter = filter,
	};
	struct sigaction sa;
	stack_t st;

	say("  child: close_range\n");
	if (syscall(__NR_close_range, 1, ~0U, 0))
		_exit(1);

	say("  child: sigaltstack\n");
	st.ss_sp = altsp;
	st.ss_flags = 0;
	st.ss_size = altsz;
	if (sigaltstack(&st, NULL) < 0) {
		say("  child: sigaltstack FAILED\n");
		_exit(11);
	}

	say("  child: sigaction(SIGSYS)\n");
	/*
	 * Deliberately as UML has it, uninitialised sa_mask included: if that
	 * is what hurts on arm64, this is where it will show.
	 */
	sa.sa_flags = SA_ONSTACK | SA_NODEFER | SA_SIGINFO;
	sa.sa_sigaction = sigsys_handler;
	sa.sa_restorer = NULL;
	if (sigaction(SIGSYS, &sa, NULL) < 0)
		_exit(2);

	say("  child: PR_SET_NO_NEW_PRIVS\n");
	prctl(PR_SET_NO_NEW_PRIVS, 1, 0, 0, 0);

	say("  child: seccomp(SET_MODE_FILTER)\n");
	if (syscall(__NR_seccomp, SECCOMP_SET_MODE_FILTER,
		    SECCOMP_FILTER_FLAG_TSYNC, &prog) != 0) {
		say("  child: seccomp FAILED\n");
		_exit(3);
	}

	say("  child: sleep(0) -- should trap\n");
	sleep(0);

	say("  child: sleep(0) returned WITHOUT trapping\n");
	_exit(4);
}

int main(int argc, char **argv)
{
	unsigned long sp;
	int pid, status;

	/* keep a usable descriptor through close_range(1, ~0U) */
	dup2(2, 0);
	setvbuf(stderr, NULL, _IONBF, 0);

	area = mmap(0, AREA, PROT_READ | PROT_WRITE, MAP_SHARED | MAP_ANON, 0, 0);
	if (area == MAP_FAILED) {
		perror("mmap");
		return 2;
	}

	/*
	 * UML puts the child's stack inside the same area, low down, and the
	 * signal stack covers the whole area. Mirror that rather than using a
	 * comfortable stack, since the layout is a candidate.
	 */
	/*
	 * mode 0: UML as it was      -- altstack is the second page only (4096)
	 * mode 1: altstack = whole area, child stack inside it
	 * mode 2: altstack = whole area, child stack in its own mapping
	 */
	{
		int mode = argc > 1 ? atoi(argv[1]) : 0;

		if (mode == 0) {
			altsp = (char *)area + 4096;
			altsz = 4096;
			sp = (unsigned long)area + 3976 - sizeof(void *);
		} else if (mode == 1) {
			altsp = area;
			altsz = AREA;
			sp = (unsigned long)area + 3976 - sizeof(void *);
		} else {
			void *st2 = mmap(0, 65536, PROT_READ | PROT_WRITE,
					 MAP_PRIVATE | MAP_ANON, -1, 0);

			altsp = area;
			altsz = AREA;
			sp = ((unsigned long)st2 + 65536) & ~15UL;
		}
		fprintf(stderr, "mode=%d altsp=%p altsz=%lu\n", mode, altsp, altsz);
	}
	fprintf(stderr, "area=%p sp=0x%lx (sp %% 16 = %lu)\n",
		area, sp, sp % 16);

	pid = clone(helper, (void *)sp, CLONE_VFORK | CLONE_VM, NULL);
	if (pid < 0) {
		perror("clone");
		return 2;
	}

	if (waitpid(pid, &status, __WCLONE) < 0) {
		perror("waitpid");
		return 2;
	}

	if (WIFEXITED(status))
		fprintf(stderr, "helper exited %d\n", WEXITSTATUS(status));
	else if (WIFSIGNALED(status))
		fprintf(stderr, "helper killed by signal %d\n", WTERMSIG(status));
	else
		fprintf(stderr, "helper status 0x%x\n", status);

	fprintf(stderr, "mctx_offset = %lu\n", mctx_offset);
	return 0;
}
