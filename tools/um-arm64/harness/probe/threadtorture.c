// SPDX-License-Identifier: GPL-2.0
/*
 * Guest thread torture for UML/arm64.
 *
 * Threads are the one piece of guest state that gates 1-5 cannot reach. Alpine's
 * busybox forks; it does not thread, and fork() passes neither a TLS value nor a
 * child-tid pointer. So the entire CLONE_SETTLS / CLONE_CHILD_CLEARTID path --
 * and with it the question of whether sys_clone's arguments are even in the
 * right order -- is untested until something linked against glibc creates a
 * thread.
 *
 * This is deliberately raw clone(2) rather than pthreads, for two reasons:
 * it needs no libc (so it can run in a 2KB initramfs next to the other probes),
 * and it pins the *kernel* ABI rather than whatever a particular libc does with
 * it. arm64 selects CLONE_BACKWARDS, so the argument order is
 *
 *	clone(flags, newsp, parent_tidptr, tls, child_tidptr)
 *
 * with tls and child_tidptr swapped relative to the asm-generic order. Getting
 * that wrong gives each thread a garbage TPIDR_EL0 and makes thread exit write
 * to a wild address, which is why the test checks both.
 *
 * What is checked, per thread:
 *   - TPIDR_EL0 equals the tls value handed to clone (CLONE_SETTLS)
 *   - it stays equal across many syscalls, i.e. threads sharing one stub
 *     process do not leak each other's thread pointer
 *   - a private FP pattern survives the same interleaving
 *   - the kernel clears the child-tid word on exit (CLONE_CHILD_CLEARTID),
 *     at the address we passed and not at the TLS address
 *
 * Build:
 *   clang --target=aarch64-linux-gnu -static -nostdlib -ffreestanding -O1 \
 *         -fno-stack-protector -o threadtorture threadtorture.c
 */

typedef unsigned long ulong;
typedef long slong;

#define SYS_write	64
#define SYS_exit	93
#define SYS_exit_group	94
#define SYS_getpid	172
#define SYS_gettid	178
#define SYS_clone	220
#define SYS_mmap	222
#define SYS_sched_yield	124
#define SYS_openat	56
#define SYS_futex	98
#define SYS_nanosleep	101
#define SYS_reboot	142

#define CLONE_VM		0x00000100
#define CLONE_FS		0x00000200
#define CLONE_FILES		0x00000400
#define CLONE_SIGHAND		0x00000800
#define CLONE_THREAD		0x00010000
#define CLONE_SETTLS		0x00080000
#define CLONE_PARENT_SETTID	0x00100000
#define CLONE_CHILD_CLEARTID	0x00200000

#define PROT_READ	0x1
#define PROT_WRITE	0x2
#define MAP_PRIVATE	0x02
#define MAP_ANONYMOUS	0x20

#define NTHREADS	8
#define STACK_SIZE	(64 * 1024)
#define ITERS		400
#define VBYTES		(32 * 16)

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
#define sys3(n, a, b, c)	sys(n, (ulong)(a), (ulong)(b), (ulong)(c), 0, 0, 0)
#define sys4(n, a, b, c, d)	sys(n, (ulong)(a), (ulong)(b), (ulong)(c), (ulong)(d), 0, 0)
#define sys6(n, a, b, c, d, e, f) \
	sys(n, (ulong)(a), (ulong)(b), (ulong)(c), (ulong)(d), (ulong)(e), (ulong)(f))

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

static void outhex(ulong v)
{
	static const char d[] = "0123456789abcdef";
	char buf[19];
	int i;

	buf[0] = '0'; buf[1] = 'x';
	for (i = 0; i < 16; i++)
		buf[2 + i] = d[(v >> ((15 - i) * 4)) & 0xf];
	buf[18] = '\0';
	out(buf);
}

static void outdec(slong v)
{
	char buf[24];
	int i = 23;

	buf[i--] = '\0';
	if (!v)
		buf[i--] = '0';
	if (v < 0) { out("-"); v = -v; }
	while (v) { buf[i--] = '0' + (v % 10); v /= 10; }
	out(&buf[i + 1]);
}

static ulong get_tpidr(void)
{
	ulong v;

	__asm__ volatile ("mrs %0, tpidr_el0" : "=r" (v));
	return v;
}

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

/* Per-thread bookkeeping, in memory shared with every thread. */
struct tinfo {
	ulong tls_want;
	volatile int ctid;	/* CLONE_CHILD_CLEARTID target */
	volatile int done;
	volatile int failed;
	volatile ulong tls_seen;
	unsigned char fp_want[VBYTES] __attribute__((aligned(16)));
	unsigned char fp_seen[VBYTES] __attribute__((aligned(16)));
};

static struct tinfo tinfo[NTHREADS];

static void fill_pattern(unsigned char *buf, ulong seed)
{
	ulong x = seed * 0x9e3779b97f4a7c15UL + 0x1234567;
	int i;

	for (i = 0; i < VBYTES; i++) {
		x ^= x << 13; x ^= x >> 7; x ^= x << 17;
		buf[i] = (unsigned char)(x ^ (i * 31));
	}
}

static int cmp(const unsigned char *a, const unsigned char *b)
{
	int i;

	for (i = 0; i < VBYTES; i++)
		if (a[i] != b[i])
			return i;
	return -1;
}

/* Runs on the new thread's own stack. Must never return. */
static void thread_body(struct tinfo *t)
{
	ulong i;

	vregs_load(t->fp_want);

	for (i = 0; i < ITERS; i++) {
		ulong tp;

		sys0(SYS_sched_yield);
		sys0(SYS_gettid);

		tp = get_tpidr();
		if (tp != t->tls_want) {
			t->tls_seen = tp;
			t->failed = 1;
			break;
		}

		vregs_store(t->fp_seen);
		if (cmp(t->fp_want, t->fp_seen) >= 0) {
			t->failed = 2;
			break;
		}
	}

	t->tls_seen = get_tpidr();
	t->done = 1;

	sys1(SYS_exit, 0);
	for (;;)
		;
}

/*
 * clone(2) with the child entering thread_body on its own stack.
 *
 * The child cannot return from this function -- it has no frame on the new
 * stack -- so the branch is done in asm and the child exits from thread_body.
 * Argument order is the CLONE_BACKWARDS one that arm64 uses.
 */
static slong spawn(struct tinfo *t, void *stack_top)
{
	register ulong x0 __asm__("x0") =
		CLONE_VM | CLONE_FS | CLONE_FILES | CLONE_SIGHAND |
		CLONE_THREAD | CLONE_SETTLS | CLONE_CHILD_CLEARTID;
	register ulong x1 __asm__("x1") = (ulong)stack_top;
	register ulong x2 __asm__("x2") = 0;			/* parent_tidptr */
	register ulong x3 __asm__("x3") = t->tls_want;		/* tls */
	register ulong x4 __asm__("x4") = (ulong)&t->ctid;	/* child_tidptr */
	register ulong x8 __asm__("x8") = SYS_clone;
	register ulong xfn __asm__("x9") = (ulong)thread_body;
	register ulong xarg __asm__("x10") = (ulong)t;

	__asm__ volatile (
		"svc #0\n"
		"cbnz x0, 1f\n"		/* parent falls through to the label */
		"mov x0, %[arg]\n"
		"blr %[fn]\n"		/* child: never returns */
		"mov x8, #93\n"		/* belt and braces: exit(1) */
		"mov x0, #1\n"
		"svc #0\n"
		"1:\n"
		: "+r" (x0)
		: "r" (x1), "r" (x2), "r" (x3), "r" (x4), "r" (x8),
		  [fn] "r" (xfn), [arg] "r" (xarg)
		/*
		 * The asm contains a blr, so every caller-saved register must be
		 * declared clobbered even though only the child ever executes it
		 * -- the compiler cannot tell the two paths apart. Omitting them
		 * lets it keep loop state in x11-x18 and produces a test whose
		 * behaviour depends on register allocation, which is exactly the
		 * kind of flake a test must not have.
		 */
		: "memory", "cc", "x5", "x6", "x7", "x11", "x12", "x13", "x14",
		  "x15", "x16", "x17", "x18", "x30");

	return (slong)x0;
}

void _start(void);

void _start(void)
{
	int i, fails = 0, started_ok = 0;
	slong r;

	r = sys3(SYS_openat, -100, "/dev/console", 1);
	if (r >= 0)
		outfd = (int)r;

	out("THREADTORTURE_START\n");

	for (i = 0; i < NTHREADS; i++) {
		void *stack;

		/*
		 * A distinctive, page-aligned, obviously-not-a-pointer TLS value
		 * per thread. If clone's arguments are swapped, TPIDR_EL0 comes
		 * out as the address of t->ctid instead, which is nothing like
		 * this pattern.
		 */
		tinfo[i].tls_want = 0x7e57000000000000UL | ((ulong)(i + 1) << 8);
		tinfo[i].ctid = 0x5a5a5a5a;
		tinfo[i].done = 0;
		tinfo[i].failed = 0;
		fill_pattern(tinfo[i].fp_want, 0x1000 + i);

		stack = (void *)sys6(SYS_mmap, 0, STACK_SIZE,
				     PROT_READ | PROT_WRITE,
				     MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
		if ((slong)stack <= 0) {
			out("THREADTORTURE_FAIL where=stack_mmap\n");
			fails++;
			break;
		}

		r = spawn(&tinfo[i], (void *)((ulong)stack + STACK_SIZE));
		out("spawn i="); outdec(i); out(" r="); outdec(r);
		out(" stack="); outhex((ulong)stack); out("\n");
		if (r < 0) {
			out("THREADTORTURE_FAIL where=clone rc="); outdec(r); out("\n");
			fails++;
			break;
		}
		started_ok++;
	}

	out("threads_spawned="); outdec(started_ok); out("\n");

	/* Wait for all of them, bounded so a hang is a failure and not a stall. */
	for (i = 0; i < 200000; i++) {
		int done = 0, j;

		for (j = 0; j < started_ok; j++)
			done += tinfo[j].done;
		if (done == started_ok)
			break;
		sys0(SYS_sched_yield);
	}

	for (i = 0; i < started_ok; i++) {
		if (!tinfo[i].done) {
			out("THREADTORTURE_FAIL where=thread_never_finished idx=");
			outdec(i); out("\n");
			fails++;
			continue;
		}
		if (tinfo[i].failed == 1) {
			out("THREADTORTURE_FAIL where=tls idx="); outdec(i);
			out(" want="); outhex(tinfo[i].tls_want);
			out(" got="); outhex(tinfo[i].tls_seen);
			out(" (a value near &ctid means clone args are swapped:"
			    " CLONE_BACKWARDS not selected)\n");
			fails++;
		} else if (tinfo[i].failed == 2) {
			out("THREADTORTURE_FAIL where=fp idx="); outdec(i); out("\n");
			fails++;
		}
	}

	/*
	 * CLONE_CHILD_CLEARTID: the kernel zeroes the word at child_tidptr when
	 * the thread exits. If tls and child_tidptr were swapped, this stays at
	 * its sentinel and some unrelated memory got zeroed instead.
	 */
	{
		int cleared = 0;

		for (i = 0; i < started_ok; i++)
			if (tinfo[i].ctid == 0)
				cleared++;
		out("child_tid_cleared="); outdec(cleared);
		out("/"); outdec(started_ok); out("\n");
		if (cleared != started_ok) {
			out("THREADTORTURE_FAIL where=child_cleartid\n");
			fails++;
		}
	}

	if (fails == 0)
		out("THREADTORTURE_OK\n");
	else
		out("THREADTORTURE_FAILED\n");

	sys4(SYS_reboot, 0xfee1deadUL, 672274793UL, 0x4321fedcUL, 0);
	sys1(SYS_exit_group, fails ? 1 : 0);
	for (;;)
		;
}
