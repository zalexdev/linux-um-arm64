// SPDX-License-Identifier: GPL-2.0
/*
 * A freestanding aarch64 probe for Android's app seccomp filter.
 *
 * The glibc version of this probe (appseccomp.c) died with SIGSYS before its
 * first printf, which is itself the finding: the kill happens inside libc's
 * startup, before main() can install a SIGSYS handler. A static glibc binary
 * issues syscalls at startup that bionic never does, and Android's app policy
 * kills on anything outside its allowlist.
 *
 * So this one has no libc at all: its own _start, raw SVC for every syscall,
 * and no startup code beyond what is written here. That makes the set of
 * syscalls it performs exactly the set listed below, which is the only way to
 * find out where the boundary is rather than guessing which of libc's dozens of
 * startup calls was fatal.
 *
 * It installs a SIGSYS handler first, before touching anything interesting.
 * Android's policy traps rather than killing outright, so the handler can
 * record si_syscall and resume -- turning each violation into a line of output.
 *
 * Build:
 *   clang --target=aarch64-linux-gnu -static -nostdlib -nostartfiles \
 *         -ffreestanding -O1 -o tinyprobe tinyprobe.c
 */

typedef unsigned long ulong;
typedef long (*sysfn)(void);

static inline long sys(long nr, long a, long b, long c, long d, long e, long f)
{
	register long x8 __asm__("x8") = nr;
	register long x0 __asm__("x0") = a;
	register long x1 __asm__("x1") = b;
	register long x2 __asm__("x2") = c;
	register long x3 __asm__("x3") = d;
	register long x4 __asm__("x4") = e;
	register long x5 __asm__("x5") = f;

	__asm__ volatile ("svc #0"
			  : "+r"(x0)
			  : "r"(x8), "r"(x1), "r"(x2), "r"(x3), "r"(x4), "r"(x5)
			  : "memory", "cc");
	return x0;
}

#define NR_write		64
#define NR_exit_group		94
#define NR_rt_sigaction		134
#define NR_rt_sigreturn		139
#define NR_ptrace		117
#define NR_memfd_create		279
#define NR_execveat		281
#define NR_seccomp		277
#define NR_prctl		167
#define NR_clone		220
#define NR_futex		98
#define NR_timer_create		107
#define NR_sched_setaffinity	122
#define NR_process_vm_readv	270
#define NR_personality		92
#define NR_madvise		233
#define NR_mmap			222
#define NR_rseq			293
#define NR_set_robust_list	99
#define NR_setpriority		140
#define NR_unshare		97
#define NR_epoll_create1	20

static void out(const char *s)
{
	const char *p = s;
	long n = 0;

	while (*p++) n++;
	sys(NR_write, 1, (long)s, n, 0, 0, 0);
}

static void outnum(long v)
{
	char buf[24];
	int i = 23;
	int neg = v < 0;

	if (neg) v = -v;
	buf[i--] = 0;
	if (!v) buf[i--] = '0';
	while (v && i >= 0) { buf[i--] = '0' + (v % 10); v /= 10; }
	if (neg && i >= 0) buf[i--] = '-';
	out(&buf[i + 1]);
}

/* Set by the handler; read after each attempt. */
static volatile long trapped;
static volatile long trapped_nr;

/*
 * The handler runs on the signal stack with no libc. It records the offending
 * syscall number from siginfo and returns; the kernel then resumes the
 * interrupted context, and because the syscall was trapped rather than
 * executed, execution continues after the SVC with whatever is in x0.
 */
static void handler(int sig, void *si, void *uc)
{
	(void)sig; (void)uc;
	trapped = 1;
	/* siginfo_t: si_signo, si_errno, si_code, then si_call_addr (ptr),
	 * si_syscall (int), si_arch (uint). si_syscall is at offset 24 on
	 * aarch64 (4 + 4 + 4 + pad + 8). */
	trapped_nr = *(int *)((char *)si + 24);
}

struct k_sigaction {
	void (*handler)(int, void *, void *);
	unsigned long flags;
	void (*restorer)(void);
	unsigned long mask[2];
};

#define SA_SIGINFO	4
#define SA_RESTORER	0x04000000
#define SA_NODEFER	0x40000000

static void restorer(void)
{
	sys(NR_rt_sigreturn, 0, 0, 0, 0, 0, 0);
}

static void try_one(const char *name, long nr, long a, long b, long c)
{
	long r;

	trapped = 0;
	r = sys(nr, a, b, c, 0, 0, 0);

	out("  ");
	out(name);
	if (trapped) {
		out("  BLOCKED (si_syscall=");
		outnum(trapped_nr);
		out(")\n");
	} else {
		out("  allowed (ret=");
		outnum(r);
		out(")\n");
	}
}

void _start(void)
{
	struct k_sigaction sa;
	int i;

	out("TINYPROBE_START\n");

	/* Install the SIGSYS handler before anything that might trip. */
	for (i = 0; i < (int)(sizeof(sa) / sizeof(long)); i++)
		((long *)&sa)[i] = 0;
	sa.handler = handler;
	sa.flags = SA_SIGINFO | SA_RESTORER | SA_NODEFER;
	sa.restorer = restorer;
	if (sys(NR_rt_sigaction, 31 /* SIGSYS */, (long)&sa, 0, 8, 0, 0) < 0) {
		out("cannot install SIGSYS handler\n");
		sys(NR_exit_group, 1, 0, 0, 0, 0, 0);
	}

	out("\nsyscalls under this process's seccomp filter:\n");

	/* What a static glibc binary does during startup, which is where the
	 * libc version of this probe died before it could say anything. */
	try_one("rseq            ", NR_rseq, 0, 0, 0);
	try_one("set_robust_list ", NR_set_robust_list, 0, 0, 0);
	try_one("personality     ", NR_personality, 0xffffffff, 0, 0);

	/* What UML needs. */
	try_one("ptrace          ", NR_ptrace, 2 /*PEEKDATA*/, 0, 0);
	try_one("process_vm_readv", NR_process_vm_readv, 0, 0, 0);
	try_one("memfd_create    ", NR_memfd_create, 0, 0, 0);
	try_one("execveat        ", NR_execveat, -1, 0, 0);
	try_one("seccomp         ", NR_seccomp, 2 /*GET_ACTION_AVAIL*/, 0, 0);
	try_one("prctl           ", NR_prctl, 38 /*NO_NEW_PRIVS*/, 0, 0);
	/* CLONE_THREAD alone is invalid: reports reachability without forking. */
	try_one("clone           ", NR_clone, 0x10000, 0, 0);
	try_one("futex           ", NR_futex, 0, 0, 0);
	try_one("timer_create    ", NR_timer_create, 1, 0, 0);
	try_one("sched_setaffinity", NR_sched_setaffinity, 0, 0, 0);
	try_one("madvise         ", NR_madvise, 0, 0, 0);
	try_one("epoll_create1   ", NR_epoll_create1, 0, 0, 0);
	try_one("setpriority     ", NR_setpriority, 0, 0, 0);
	try_one("unshare         ", NR_unshare, 0, 0, 0);

	out("\nTINYPROBE_DONE\n");
	sys(NR_exit_group, 0, 0, 0, 0, 0, 0);
}
