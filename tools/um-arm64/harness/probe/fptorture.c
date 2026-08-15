// SPDX-License-Identifier: GPL-2.0
/*
 * FP/SIMD state torture test for the UML/arm64 guest.
 *
 * Why this exists
 * ---------------
 * Gates 1-5 barely touch floating point. busybox and ash compute almost
 * nothing, so a bug in the FP save/restore path survives every one of them and
 * only surfaces under dpkg and a compiler, where NEON is used continuously --
 * as "apt occasionally fails with a strange error". The arch_switch_to() bug
 * that clobbered guest TLS was the same class of defect: state that is not part
 * of the GPR set, and therefore not covered by anything that only tests
 * control flow.
 *
 * There are three distinct paths that can corrupt guest FP state, and this
 * exercises all three separately so a failure names the guilty one:
 *
 *   1. syscall round-trip  -- get_fp_registers()/put_fp_registers() in the
 *                             ptrace path, or the mcontext copy in seccomp mode
 *   2. task switch         -- two tasks with different FP state interleaved
 *   3. signal delivery     -- fpsimd_context marshalled into and out of
 *                             mcontext.__reserved[] by arch/arm64/um/signal.c,
 *                             which is hand-written and entirely untested by
 *                             anything that does not take a signal
 *
 * Comparison is bit-exact over all 32 V registers plus FPCR and FPSR. A test
 * that only checked arithmetic results would miss a swapped register pair or a
 * truncated high half.
 *
 * Build:
 *   clang --target=aarch64-linux-gnu -static -nostdlib -ffreestanding -O1 \
 *         -fno-stack-protector -o fptorture fptorture.c
 */

typedef unsigned long ulong;
typedef long slong;
typedef unsigned int uint;

#define SYS_write	64
#define SYS_exit_group	94
#define SYS_getpid	172
#define SYS_gettid	178
#define SYS_clone	220
#define SYS_wait4	260
#define SYS_kill	129
#define SYS_rt_sigaction 134
#define SYS_sched_yield	124
#define SYS_openat	56
#define SYS_nanosleep	101

#define SIGUSR1		10
#define SA_SIGINFO	4
#define SA_ONSTACK	0x08000000
#define SA_NODEFER	0x40000000

static inline slong sys(slong nr, ulong a, ulong b, ulong c,
			ulong d, ulong e, ulong f)
{
	register slong x8 __asm__("x8") = nr;
	register ulong x0 __asm__("x0") = a;
	register ulong x1 __asm__("x1") = b;
	register ulong x2 __asm__("x2") = c;
	register ulong x3 __asm__("x3") = d;
	register ulong x4 __asm__("x4") = e;
	register ulong x5 __asm__("x5") = f;

	__asm__ volatile ("svc #0"
		: "+r" (x0)
		: "r" (x8), "r" (x1), "r" (x2), "r" (x3), "r" (x4), "r" (x5)
		: "memory");
	return (slong)x0;
}

#define sys0(n)			sys(n, 0, 0, 0, 0, 0, 0)
#define sys1(n, a)		sys(n, (ulong)(a), 0, 0, 0, 0, 0)
#define sys2(n, a, b)		sys(n, (ulong)(a), (ulong)(b), 0, 0, 0, 0)
#define sys3(n, a, b, c)	sys(n, (ulong)(a), (ulong)(b), (ulong)(c), 0, 0, 0)
#define sys4(n, a, b, c, d)	sys(n, (ulong)(a), (ulong)(b), (ulong)(c), (ulong)(d), 0, 0)

static int outfd = 1;

static ulong slen(const char *s)
{
	ulong n = 0;

	while (s[n])
		n++;
	return n;
}

static void out(const char *s)
{
	sys3(SYS_write, outfd, s, slen(s));
}

static void outdec(slong v)
{
	char buf[24];
	int i = 23;
	int neg = 0;

	buf[i--] = '\0';
	if (v < 0) { neg = 1; v = -v; }
	if (!v)
		buf[i--] = '0';
	while (v) {
		buf[i--] = '0' + (v % 10);
		v /= 10;
	}
	if (neg)
		buf[i--] = '-';
	out(&buf[i + 1]);
}

static void outhex(ulong v)
{
	static const char d[] = "0123456789abcdef";
	char buf[19];
	int i;

	buf[0] = '0';
	buf[1] = 'x';
	for (i = 0; i < 16; i++)
		buf[2 + i] = d[(v >> ((15 - i) * 4)) & 0xf];
	buf[18] = '\0';
	out(buf);
}

static void kv(const char *k, slong v)
{
	out(k); out("="); outdec(v); out("\n");
}

/* ------------------------------------------------------------------ */
/* V register access                                                   */
/* ------------------------------------------------------------------ */

/*
 * 32 registers x 16 bytes. Stored and loaded with ld1/st1 in four-register
 * groups, which is the only way to touch all of them without the compiler
 * deciding to allocate some of them for itself.
 */
#define NVREG	32
#define VBYTES	(NVREG * 16)

static void vregs_store(void *p)
{
	__asm__ volatile (
		"st1 {v0.16b - v3.16b},   [%0], #64\n"
		"st1 {v4.16b - v7.16b},   [%0], #64\n"
		"st1 {v8.16b - v11.16b},  [%0], #64\n"
		"st1 {v12.16b - v15.16b}, [%0], #64\n"
		"st1 {v16.16b - v19.16b}, [%0], #64\n"
		"st1 {v20.16b - v23.16b}, [%0], #64\n"
		"st1 {v24.16b - v27.16b}, [%0], #64\n"
		"st1 {v28.16b - v31.16b}, [%0], #64\n"
		: "+r" (p) :: "memory");
}

static void vregs_load(const void *p)
{
	__asm__ volatile (
		"ld1 {v0.16b - v3.16b},   [%0], #64\n"
		"ld1 {v4.16b - v7.16b},   [%0], #64\n"
		"ld1 {v8.16b - v11.16b},  [%0], #64\n"
		"ld1 {v12.16b - v15.16b}, [%0], #64\n"
		"ld1 {v16.16b - v19.16b}, [%0], #64\n"
		"ld1 {v20.16b - v23.16b}, [%0], #64\n"
		"ld1 {v24.16b - v27.16b}, [%0], #64\n"
		"ld1 {v28.16b - v31.16b}, [%0], #64\n"
		: "+r" (p) :: "memory",
		  "v0","v1","v2","v3","v4","v5","v6","v7",
		  "v8","v9","v10","v11","v12","v13","v14","v15",
		  "v16","v17","v18","v19","v20","v21","v22","v23",
		  "v24","v25","v26","v27","v28","v29","v30","v31");
}

static ulong get_fpcr(void)
{
	ulong v;

	__asm__ volatile ("mrs %0, fpcr" : "=r" (v));
	return v;
}

static void set_fpcr(ulong v)
{
	__asm__ volatile ("msr fpcr, %0" :: "r" (v));
}

static ulong get_tpidr(void)
{
	ulong v;

	__asm__ volatile ("mrs %0, tpidr_el0" : "=r" (v));
	return v;
}

static void set_tpidr(ulong v)
{
	__asm__ volatile ("msr tpidr_el0, %0" :: "r" (v));
}

/* Only the NZCV bits of PSTATE; the rest is not writable from EL0. */
static ulong get_nzcv(void)
{
	ulong v;

	__asm__ volatile ("mrs %0, nzcv" : "=r" (v));
	return v & 0xf0000000UL;
}

static void set_nzcv(ulong v)
{
	__asm__ volatile ("msr nzcv, %0" :: "r" (v & 0xf0000000UL));
}

static ulong get_fpsr(void)
{
	ulong v;

	__asm__ volatile ("mrs %0, fpsr" : "=r" (v));
	return v;
}

static void set_fpsr(ulong v)
{
	__asm__ volatile ("msr fpsr, %0" :: "r" (v));
}

/* ------------------------------------------------------------------ */

static unsigned char expect[VBYTES] __attribute__((aligned(16)));
static unsigned char actual[VBYTES] __attribute__((aligned(16)));

/* A distinct, non-repeating byte for every position, so a swapped register
 * pair or a truncated 64-bit half is visible, not just a zeroed register. */
static void fill_pattern(unsigned char *buf, ulong seed)
{
	ulong x = seed * 0x9e3779b97f4a7c15UL + 0x1234567;
	int i;

	for (i = 0; i < VBYTES; i++) {
		x ^= x << 13;
		x ^= x >> 7;
		x ^= x << 17;
		buf[i] = (unsigned char)(x ^ (i * 31));
	}
}

/* Returns the index of the first differing byte, or -1. */
static int cmp_pattern(const unsigned char *a, const unsigned char *b)
{
	int i;

	for (i = 0; i < VBYTES; i++)
		if (a[i] != b[i])
			return i;
	return -1;
}

static void report_mismatch(const char *where, int off, ulong iter)
{
	out("FPTORTURE_FAIL where="); out(where);
	out(" iter="); outdec((slong)iter);
	out(" vreg=v"); outdec(off / 16);
	out(" byte="); outdec(off % 16);
	out(" expect="); outhex(expect[off]);
	out(" actual="); outhex(actual[off]);
	out("\n");
}

/* ------------------------------------------------------------------ */
/* signal handler: clobbers every V register, then returns              */
/* ------------------------------------------------------------------ */

static volatile int sig_seen;
static unsigned char sig_junk[VBYTES] __attribute__((aligned(16)));

static void usr1_handler(int sig)
{
	sig_seen++;
	/*
	 * Destroy all FP state from inside the handler. If the kernel's
	 * signal frame did not save it, or restores it wrongly on sigreturn,
	 * the caller's comparison fails immediately afterwards. Restoring
	 * clobbered state across a handler is exactly what fpsimd_context in
	 * mcontext.__reserved[] is for.
	 */
	vregs_load(sig_junk);
	set_fpcr(0);
	/*
	 * Disturb the other pieces of non-GPR state too. If the signal frame
	 * does not carry them, the checks after the handler returns catch it.
	 * TPIDR_EL0 is deliberately left alone: it is per-thread state that a
	 * handler shares with its interrupted context on real hardware, so
	 * clobbering it here would be testing the wrong contract.
	 */
	set_nzcv(0xf0000000UL);
}

/* Kernel struct sigaction: arm64 defines SA_RESTORER, so sa_restorer sits
 * between the flags and the mask. SA_RESTORER is deliberately *not* set, which
 * forces signal return through the vDSO's __kernel_rt_sigreturn -- the path
 * real glibc uses, and one nothing else in this test suite exercises. */
struct k_sigaction {
	void *handler;
	ulong flags;
	void *restorer;
	ulong mask;
};

static int install_handler(void)
{
	struct k_sigaction act = { 0 };

	act.handler = (void *)usr1_handler;
	act.flags = SA_NODEFER;
	act.restorer = 0;
	act.mask = 0;

	return (int)sys4(SYS_rt_sigaction, SIGUSR1, &act, 0, 8);
}

/* ------------------------------------------------------------------ */

void _start(void);

void _start(void)
{
	ulong iters = 4000;
	ulong fpcr_set = 0x0000000000000000UL;  /* filled in below */
	ulong fpcr_seen, fpsr_seen;
	slong r, pid, mypid;
	ulong i;
	int off, fails = 0;

	r = sys3(SYS_openat, -100, "/dev/console", 1);
	if (r >= 0)
		outfd = (int)r;

	out("FPTORTURE_START\n");
	mypid = sys0(SYS_getpid);
	kv("pid", mypid);

	fill_pattern(expect, 0xabcdef);
	fill_pattern(sig_junk, 0x555555);

	/*
	 * FPCR bits that are architecturally writable and stick: RMode (22-23),
	 * FZ (24), DN (25). Setting a non-default rounding mode means a lost
	 * FPCR shows up as a changed value, not just as zero.
	 */
	fpcr_set = (1UL << 22) | (1UL << 24) | (1UL << 25);

	/* ---------------- phase 1: syscall round-trip ---------------- */
	vregs_load(expect);
	set_fpcr(fpcr_set);
	set_fpsr(0);

	for (i = 0; i < iters; i++) {
		sys0(SYS_getpid);          /* forces a guest->UML->guest round trip */
		vregs_store(actual);
		off = cmp_pattern(expect, actual);
		if (off >= 0) {
			report_mismatch("syscall", off, i);
			fails++;
			break;
		}
	}
	fpcr_seen = get_fpcr();
	if (!fails)
		out("phase1_syscall=ok\n");
	if ((fpcr_seen & fpcr_set) != fpcr_set) {
		out("FPTORTURE_FAIL where=fpcr_syscall expect=");
		outhex(fpcr_set); out(" actual="); outhex(fpcr_seen); out("\n");
		fails++;
	} else {
		out("phase1_fpcr=ok\n");
	}

	/* ---------------- phase 2: task switches ---------------- */
	/*
	 * A child with a *different* FP pattern, both looping. sched_yield on
	 * each side maximises the number of switches between two live FP
	 * contexts, which is what makes a shared-state bug show up.
	 */
	pid = sys(SYS_clone, 17 /* SIGCHLD */, 0, 0, 0, 0, 0);
	if (pid == 0) {
		static unsigned char cexp[VBYTES] __attribute__((aligned(16)));
		static unsigned char cact[VBYTES] __attribute__((aligned(16)));
		int coff;
		ulong j;

		fill_pattern(cexp, 0x13579b);
		vregs_load(cexp);
		set_fpcr(0);
		for (j = 0; j < iters; j++) {
			sys0(SYS_sched_yield);
			vregs_store(cact);
			coff = cmp_pattern(cexp, cact);
			if (coff >= 0)
				sys1(SYS_exit_group, 3);
		}
		sys1(SYS_exit_group, 0);
	}

	vregs_load(expect);
	set_fpcr(fpcr_set);
	for (i = 0; i < iters; i++) {
		sys0(SYS_sched_yield);
		vregs_store(actual);
		off = cmp_pattern(expect, actual);
		if (off >= 0) {
			report_mismatch("taskswitch", off, i);
			fails++;
			break;
		}
	}
	{
		int status = 0;

		sys4(SYS_wait4, pid, &status, 0, 0);
		if (status != 0) {
			out("FPTORTURE_FAIL where=taskswitch_child status=");
			outhex((ulong)(uint)status); out("\n");
			fails++;
		} else if (!fails) {
			out("phase2_taskswitch=ok\n");
		}
	}
	fpcr_seen = get_fpcr();
	if ((fpcr_seen & fpcr_set) != fpcr_set) {
		out("FPTORTURE_FAIL where=fpcr_taskswitch actual=");
		outhex(fpcr_seen); out("\n");
		fails++;
	} else {
		out("phase2_fpcr=ok\n");
	}

	/* ---------------- phase 3: signal delivery ---------------- */
	r = install_handler();
	if (r < 0) {
		out("FPTORTURE_FAIL where=rt_sigaction rc="); outdec(r); out("\n");
		fails++;
	} else {
		ulong sigiters = 500;
		int sigfail = 0;

		vregs_load(expect);
		set_fpcr(fpcr_set);
		set_fpsr(0);

		for (i = 0; i < sigiters; i++) {
			sys2(SYS_kill, mypid, SIGUSR1);
			vregs_store(actual);
			off = cmp_pattern(expect, actual);
			if (off >= 0) {
				report_mismatch("signal", off, i);
				sigfail = 1;
				fails++;
				break;
			}
			fpcr_seen = get_fpcr();
			if ((fpcr_seen & fpcr_set) != fpcr_set) {
				out("FPTORTURE_FAIL where=fpcr_signal iter=");
				outdec((slong)i);
				out(" actual="); outhex(fpcr_seen); out("\n");
				sigfail = 1;
				fails++;
				break;
			}
		}
		kv("signals_delivered", sig_seen);
		if (!sigfail && sig_seen > 0)
			out("phase3_signal=ok\n");
		else if (sig_seen == 0) {
			out("FPTORTURE_FAIL where=signal_never_delivered\n");
			fails++;
		}
	}

	/* ---------------- phase 4: TPIDR_EL0 (guest TLS) ---------------- */
	/*
	 * The TLS clobber that broke musl was found only because musl asserts on
	 * it. Nothing in gates 1-5 checks TPIDR_EL0 directly, so a regression
	 * would again be reported as "the dynamic loader crashes", which is a
	 * much longer path to the cause. Check it explicitly, across all three
	 * transitions.
	 */
	{
		ulong tp_want = 0xdeadbeefcafe0000UL;
		ulong tp_seen;
		int tlsfail = 0;

		set_tpidr(tp_want);

		for (i = 0; i < 1000; i++) {
			sys0(SYS_getpid);
			tp_seen = get_tpidr();
			if (tp_seen != tp_want) {
				out("FPTORTURE_FAIL where=tls_syscall iter=");
				outdec((slong)i); out(" want="); outhex(tp_want);
				out(" got="); outhex(tp_seen); out("\n");
				tlsfail = 1; fails++; break;
			}
		}
		for (i = 0; !tlsfail && i < 1000; i++) {
			sys0(SYS_sched_yield);
			tp_seen = get_tpidr();
			if (tp_seen != tp_want) {
				out("FPTORTURE_FAIL where=tls_yield iter=");
				outdec((slong)i); out(" got="); outhex(tp_seen); out("\n");
				tlsfail = 1; fails++; break;
			}
		}
		for (i = 0; !tlsfail && i < 200; i++) {
			sys2(SYS_kill, mypid, SIGUSR1);
			tp_seen = get_tpidr();
			if (tp_seen != tp_want) {
				out("FPTORTURE_FAIL where=tls_signal iter=");
				outdec((slong)i); out(" got="); outhex(tp_seen); out("\n");
				tlsfail = 1; fails++; break;
			}
		}
		if (!tlsfail)
			out("phase4_tls=ok\n");
	}

	/* ---------------- phase 5: PSTATE condition flags ---------------- */
	/*
	 * NZCV must survive signal delivery: the handler runs arbitrary code and
	 * sigreturn is required to put the interrupted flags back, or a
	 * conditional branch straddling a signal takes the wrong arm. This is
	 * the one part of PSTATE the port deliberately allows a sigreturn frame
	 * to set (see UM_PSTATE_WRITABLE), so it is also the part most likely to
	 * be got wrong.
	 */
	{
		int nzfail = 0;
		ulong want, got;

		for (i = 0; i < 4 && !nzfail; i++) {
			/* Exercise each flag bit combination we can set cheaply. */
			want = (i & 1) ? (1UL << 30) : 0;        /* Z */
			want |= (i & 2) ? (1UL << 29) : 0;       /* C */

			set_nzcv(want);
			sys2(SYS_kill, mypid, SIGUSR1);
			got = get_nzcv();
			if (got != want) {
				out("FPTORTURE_FAIL where=nzcv_signal case=");
				outdec((slong)i);
				out(" want="); outhex(want);
				out(" got="); outhex(got); out("\n");
				nzfail = 1; fails++;
			}
		}
		if (!nzfail)
			out("phase5_nzcv=ok\n");
	}

	fpsr_seen = get_fpsr();
	out("fpsr_final="); outhex(fpsr_seen); out("\n");

	if (fails == 0)
		out("FPTORTURE_OK\n");
	else
		out("FPTORTURE_FAILED\n");

	sys1(SYS_exit_group, fails ? 1 : 0);
	for (;;)
		;
}
