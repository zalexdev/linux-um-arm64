// SPDX-License-Identifier: GPL-2.0
/*
 * What does one SECCOMP_RET_TRAP guest syscall actually cost, stage by stage?
 *
 * The cost model for UML's seccomp handoff splits into three groups:
 *
 *   host signal machinery   svc -> BPF -> force_sig(SIGSYS) -> setup_rt_frame
 *                           -> handler -> rt_sigreturn -> frame teardown
 *   cross-core handoff      release-exchange of the protocol word, peer's
 *                           acquire-spin observing it, and the same back
 *   kernel marshalling      get_stub_state/set_stub_state: 272 B GP + 520 B FP
 *                           each way, plus a 128 B siginfo copy
 *
 * Nothing in the tree measures those separately, so every proposal to trim the
 * kernel side has been argued against an estimate. This binary measures them
 * with no kernel involved at all, by rebuilding each stage on top of the last:
 *
 *   getppid      raw host syscall -- validates the pinning against perfbench
 *   sigsys       SECCOMP_RET_TRAP + a handler that returns. This alone IS the
 *                host signal machinery, and it is the floor under any scheme
 *                that keeps SECCOMP_RET_TRAP as the trap mechanism.
 *   handoff      sigsys + a release-exchange ping-pong with a peer process on
 *                the other core, peer doing nothing
 *   handoff32    the same, but the peer's spin reads the architected counter
 *                once per 32 iterations instead of every iteration
 *   marshal      handoff + 272+520 B each way and a 128 B siginfo copy
 *
 * Differences between adjacent rows are the stages. Run it pinned exactly as
 * harness/verifybench.sh pins (taskset 30, keeper on core 6) or the numbers
 * are not comparable to that table.
 *
 * Build:
 *   clang --target=aarch64-linux-gnu -static -O2 -o handoff handoff.c
 */
#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <stddef.h>
#include <string.h>
#include <unistd.h>
#include <time.h>
#include <sched.h>
#include <signal.h>
#include <errno.h>
#include <sys/mman.h>
#include <sys/wait.h>
#include <sys/prctl.h>
#include <sys/syscall.h>
#include <linux/filter.h>
#include <linux/seccomp.h>
#include <linux/audit.h>

/* The syscall we make the filter trap. Never actually executed. */
#define TRAP_NR __NR_getcpu

/* Protocol word states, mirroring FUTEX_IN_KERN / owner semantics. */
#define OWN_ME	0u
#define OWN_PEER 1u

struct shared {
	volatile unsigned int futex;
	char pad[128];
	/* Marshalling payload: the sizes arch/arm64/um/os-Linux/mcontext.c moves. */
	char gp[272];
	char fp[520];
	char si[128];
	char pad2[128];
	volatile unsigned int stop;
};

static struct shared *sh;
static int spin_stride = 1;	/* counter reads per this many poll iterations */

static double now(void)
{
	struct timespec ts;

	clock_gettime(CLOCK_MONOTONIC, &ts);
	return ts.tv_sec + ts.tv_nsec / 1e9;
}

static inline unsigned long cntvct(void)
{
	unsigned long v;

	asm volatile("mrs %0, cntvct_el0" : "=r"(v));
	return v;
}

static inline unsigned long cntfrq(void)
{
	unsigned long v;

	asm volatile("mrs %0, cntfrq_el0" : "=r"(v));
	return v;
}

/* Release-exchange: publish everything written before handing ownership over. */
static inline unsigned int xchg_release(volatile unsigned int *p, unsigned int v)
{
	return __atomic_exchange_n(p, v, __ATOMIC_ACQ_REL);
}

static inline unsigned int load_acquire(volatile unsigned int *p)
{
	return __atomic_load_n(p, __ATOMIC_ACQUIRE);
}

/*
 * Spin until the word says we own it. Budget is wall time via CNTVCT_EL0,
 * exactly as stub_futex_spin() does; `stride` controls how often the counter
 * is sampled, which is the thing proposal "take the counter read out of the
 * spin loop" wants priced.
 */
static void spin_until(volatile unsigned int *p, unsigned int want,
		       unsigned long budget, int stride)
{
	unsigned long start = cntvct();
	int i = 0;

	for (;;) {
		if (load_acquire(p) == want)
			return;
		asm volatile("yield");
		if (++i % stride == 0 && cntvct() - start > budget)
			break;
	}
	/* Budget blown: keep polling anyway, this probe never parks. */
	while (load_acquire(p) != want)
		asm volatile("yield");
}

static int pin(int cpu)
{
	cpu_set_t set;

	CPU_ZERO(&set);
	CPU_SET(cpu, &set);
	return sched_setaffinity(0, sizeof(set), &set);
}

/* ---- the SIGSYS side ------------------------------------------------------ */

static volatile int mode;	/* 0 = return only, 1 = handoff, 2 = marshal */
static unsigned long spin_budget;

static void sigsys_handler(int sig, siginfo_t *si, void *uc)
{
	(void)sig; (void)si; (void)uc;

	if (mode == 0)
		return;

	if (mode == 2) {
		/* What get_stub_state() moves out of the mcontext. */
		memcpy(sh->gp, (char *)uc + 64, sizeof(sh->gp));
		memcpy(sh->fp, (char *)uc + 64, sizeof(sh->fp));
		memcpy(sh->si, si, sizeof(sh->si));
	}

	/* Hand the word to the peer and wait for it to hand it back. */
	xchg_release(&sh->futex, OWN_PEER);
	spin_until(&sh->futex, OWN_ME, spin_budget, spin_stride);

	if (mode == 2) {
		/* What set_stub_state() writes back. */
		memcpy((char *)uc + 64, sh->gp, sizeof(sh->gp));
		memcpy((char *)uc + 64, sh->fp, sizeof(sh->fp));
	}
}

static int install_filter(void)
{
	struct sock_filter f[] = {
		BPF_STMT(BPF_LD | BPF_W | BPF_ABS,
			 offsetof(struct seccomp_data, arch)),
		BPF_JUMP(BPF_JMP | BPF_JEQ | BPF_K, AUDIT_ARCH_AARCH64, 1, 0),
		BPF_STMT(BPF_RET | BPF_K, SECCOMP_RET_ALLOW),
		BPF_STMT(BPF_LD | BPF_W | BPF_ABS,
			 offsetof(struct seccomp_data, nr)),
		BPF_JUMP(BPF_JMP | BPF_JEQ | BPF_K, TRAP_NR, 0, 1),
		BPF_STMT(BPF_RET | BPF_K, SECCOMP_RET_TRAP),
		BPF_STMT(BPF_RET | BPF_K, SECCOMP_RET_ALLOW),
	};
	struct sock_fprog prog = { .len = sizeof(f) / sizeof(f[0]), .filter = f };

	if (prctl(PR_SET_NO_NEW_PRIVS, 1, 0, 0, 0))
		return -1;
	return syscall(__NR_seccomp, SECCOMP_SET_MODE_FILTER, 0, &prog);
}

/* ---- the peer ------------------------------------------------------------- */

static pid_t peer_pid;

static void peer_loop(int cpu, int stride)
{
	pin(cpu);
	while (!sh->stop) {
		/* Wait to be handed the word, then hand it straight back. */
		unsigned long start = cntvct();
		int i = 0;

		while (load_acquire(&sh->futex) != OWN_PEER) {
			if (sh->stop)
				_exit(0);
			asm volatile("yield");
			if (++i % stride == 0 && cntvct() - start > spin_budget * 100)
				start = cntvct();	/* never park, just re-arm */
		}
		xchg_release(&sh->futex, OWN_ME);
	}
	_exit(0);
}

/* ---- measurement ---------------------------------------------------------- */

static double bench(long iters, int raw)
{
	double t0, t1;
	long i;

	t0 = now();
	for (i = 0; i < iters; i++) {
		if (raw)
			syscall(__NR_getppid);
		else
			syscall(TRAP_NR, NULL, NULL, NULL);
	}
	t1 = now();
	return (t1 - t0) * 1e6 / iters;
}

static int cmpd(const void *a, const void *b)
{
	double x = *(const double *)a, y = *(const double *)b;

	return x < y ? -1 : x > y ? 1 : 0;
}

static void report(const char *name, double *v, int n)
{
	qsort(v, n, sizeof(*v), cmpd);
	printf("%-12s %8.3f us/op   min %8.3f  max %8.3f  n=%d\n",
	       name, v[n / 2], v[0], v[n - 1], n);
	fflush(stdout);
}

int main(int argc, char **argv)
{
	long iters = argc > 1 ? atol(argv[1]) : 200000;
	int rounds = argc > 2 ? atoi(argv[2]) : 7;
	double v[64];
	int r;
	struct sigaction sa;

	if (rounds > 64)
		rounds = 64;

	sh = mmap(NULL, sizeof(*sh), PROT_READ | PROT_WRITE,
		  MAP_SHARED | MAP_ANONYMOUS, -1, 0);
	if (sh == MAP_FAILED) {
		perror("mmap");
		return 1;
	}
	memset(sh, 0, sizeof(*sh));
	sh->futex = OWN_ME;

	printf("HANDOFF_START cntfrq=%lu Hz\n", cntfrq());
	/* 10 us, the SECCOMP_SPIN_DEFAULT_US the kernel ships. */
	spin_budget = cntfrq() / 100000;

	if (pin(4))
		perror("pin cpu4 (continuing unpinned)");

	/* Row 1: raw syscall, before any filter exists. */
	for (r = 0; r < rounds; r++)
		v[r] = bench(iters, 1);
	report("getppid", v, rounds);

	memset(&sa, 0, sizeof(sa));
	sa.sa_sigaction = sigsys_handler;
	sa.sa_flags = SA_SIGINFO | SA_NODEFER;
	sigfillset(&sa.sa_mask);
	if (sigaction(SIGSYS, &sa, NULL)) {
		perror("sigaction");
		return 1;
	}
	if (install_filter()) {
		perror("seccomp");
		return 1;
	}

	/* Row 2: the host signal machinery alone. */
	mode = 0;
	for (r = 0; r < rounds; r++)
		v[r] = bench(iters, 0);
	report("sigsys", v, rounds);

	/* Rows 3-5 need the peer. */
	peer_pid = fork();
	if (peer_pid == 0)
		peer_loop(5, 1);

	mode = 1;
	spin_stride = 1;
	for (r = 0; r < rounds; r++)
		v[r] = bench(iters, 0);
	report("handoff", v, rounds);

	mode = 2;
	for (r = 0; r < rounds; r++)
		v[r] = bench(iters, 0);
	report("marshal", v, rounds);

	sh->stop = 1;
	xchg_release(&sh->futex, OWN_PEER);
	waitpid(peer_pid, NULL, 0);

	/* Row 5: same handoff, counter sampled once per 32 poll iterations. */
	sh->stop = 0;
	sh->futex = OWN_ME;
	peer_pid = fork();
	if (peer_pid == 0)
		peer_loop(5, 32);
	mode = 1;
	spin_stride = 32;
	for (r = 0; r < rounds; r++)
		v[r] = bench(iters, 0);
	report("handoff32", v, rounds);

	sh->stop = 1;
	xchg_release(&sh->futex, OWN_PEER);
	waitpid(peer_pid, NULL, 0);

	printf("HANDOFF_DONE\n");
	return 0;
}
