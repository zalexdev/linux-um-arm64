// SPDX-License-Identifier: GPL-2.0
/*
 * Which syscalls does Android's app seccomp filter forbid?
 *
 * A UML kernel launched from an adb shell works. The same binary launched by an
 * app dies immediately with status 159 -- 128 + SIGSYS -- in both ptrace and
 * seccomp mode. The reason is not anything UML does: seccomp filters are
 * inherited across fork and exec, so a process the app spawns runs under the
 * filter zygote installed on the app itself, and that filter kills on syscalls
 * outside its allowlist. `run-as` does not carry that filter, which is why
 * everything looked fine when tested that way.
 *
 * "Something is denied" is not actionable; which syscall is. Android's app
 * policy traps rather than kills outright, so a SIGSYS handler can catch the
 * violation, read si_syscall, and jump back -- turning a fatal signal into a
 * line of output. That makes it possible to enumerate the whole boundary in one
 * run instead of one rebuild per guess.
 *
 * Build:
 *   clang --target=aarch64-linux-gnu -static -O1 -o appseccomp appseccomp.c
 * Ship it in the APK as a lib*.so and run it from the app, because that is the
 * only context where the filter is in force.
 */
#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>
#include <setjmp.h>
#include <signal.h>
#include <fcntl.h>
#include <sched.h>
#include <sys/mman.h>
#include <sys/prctl.h>
#include <sys/ptrace.h>
#include <sys/syscall.h>
#include <sys/uio.h>
#include <sys/wait.h>
#include <linux/seccomp.h>

static sigjmp_buf jb;
static volatile sig_atomic_t trapped;
static volatile sig_atomic_t trapped_nr;

static void sigsys_handler(int sig, siginfo_t *si, void *uc)
{
	(void)sig; (void)uc;
	trapped = 1;
	trapped_nr = si->si_syscall;
	siglongjmp(jb, 1);
}

#define TRY(name, expr)							\
	do {								\
		trapped = 0;						\
		if (sigsetjmp(jb, 1) == 0) {				\
			errno = 0;					\
			long _r = (long)(expr);				\
			printf("  %-22s allowed   (ret=%ld errno=%d)\n",\
			       name, _r, errno);			\
		} else {						\
			printf("  %-22s BLOCKED   (si_syscall=%d)\n",	\
			       name, (int)trapped_nr);			\
			blocked++;					\
		}							\
	} while (0)

int main(void)
{
	struct sigaction sa;
	int blocked = 0;
	int fd;

	setvbuf(stdout, NULL, _IONBF, 0);
	printf("APPSECCOMP_START uid=%d\n", (int)getuid());

	memset(&sa, 0, sizeof(sa));
	sa.sa_sigaction = sigsys_handler;
	sa.sa_flags = SA_SIGINFO | SA_NODEFER;
	sigemptyset(&sa.sa_mask);
	if (sigaction(SIGSYS, &sa, NULL)) {
		printf("cannot install SIGSYS handler: %d\n", errno);
		return 1;
	}

	printf("\nsyscalls UML needs, under this process's seccomp filter:\n");

	/* The ptrace path. */
	TRY("ptrace(PEEKDATA)", ptrace(PTRACE_PEEKDATA, 0, NULL, NULL));
	TRY("process_vm_readv", syscall(SYS_process_vm_readv, 0, NULL, 0, NULL, 0, 0));

	/* How the stub is created and started. */
	TRY("memfd_create", syscall(SYS_memfd_create, "t", 0));
	TRY("execveat(-1,\"\")", syscall(SYS_execveat, -1, "", NULL, NULL, 0));

	/* Seccomp mode. */
	TRY("prctl(NO_NEW_PRIVS)", prctl(PR_SET_NO_NEW_PRIVS, 1, 0, 0, 0));
	TRY("seccomp(GET_ACTION)", syscall(SYS_seccomp, SECCOMP_GET_ACTION_AVAIL, 0, NULL));

	/* Memory and timing, used constantly by the kernel side. */
	TRY("mmap(FIXED)", mmap(NULL, 4096, PROT_READ, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0));
	TRY("madvise(DONTNEED)", madvise(NULL, 0, MADV_DONTNEED));
	TRY("timer_create", syscall(SYS_timer_create, 1, NULL, NULL));
	TRY("timerfd_create", syscall(SYS_timerfd_create, 1, 0));
	TRY("sched_setaffinity", syscall(SYS_sched_setaffinity, 0, 0, NULL));
	TRY("futex(WAIT,0)", syscall(SYS_futex, NULL, 0, 0, NULL, NULL, 0));
	TRY("epoll_create1", syscall(SYS_epoll_create1, 0));
	TRY("personality", syscall(SYS_personality, 0xffffffff));
	TRY("setpriority", syscall(SYS_setpriority, 0, 0, 0));

	/* Things passt wants. */
	TRY("unshare(0)", syscall(SYS_unshare, 0));
	TRY("socket(AF_UNIX)", syscall(SYS_socket, 1, 2, 0));

	/* clone with CLONE_VM is how the stub process is made; do it for real
	 * in a child so a failure cannot take this process with it. */
	fd = fork();
	if (fd == 0) {
		trapped = 0;
		if (sigsetjmp(jb, 1) == 0) {
			pid_t p = syscall(SYS_clone, CLONE_VM | CLONE_VFORK | SIGCHLD,
					  NULL, NULL, NULL, NULL);
			if (p == 0)
				_exit(0);
			_exit(p < 0 ? 2 : 0);
		}
		_exit(3);
	} else if (fd > 0) {
		int st = 0;

		waitpid(fd, &st, 0);
		if (WIFSIGNALED(st) && WTERMSIG(st) == SIGSYS)
			printf("  %-22s BLOCKED   (killed child with SIGSYS)\n",
			       "clone(CLONE_VM)"), blocked++;
		else
			printf("  %-22s allowed   (child status 0x%x)\n",
			       "clone(CLONE_VM)", st);
	}

	printf("\nAPPSECCOMP_DONE blocked=%d\n", blocked);
	return 0;
}
