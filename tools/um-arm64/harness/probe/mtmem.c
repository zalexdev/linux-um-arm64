// SPDX-License-Identifier: GPL-2.0
/*
 * Multi-threaded memory integrity torture for UML/arm64.
 *
 * Every existing probe covers one axis. threadtorture runs eight threads but
 * touches a few kilobytes; mmtorture moves 16 MB but is single-threaded;
 * uacctorture moves 48 MB through a pipe from a *forked* child, which has its
 * own mm. The gate-7 workload sits in the one cell none of them occupy: many
 * threads sharing a single mm -- and therefore, under UML's SKAS model, sharing
 * a single stub process -- while several tens of megabytes of anonymous memory
 * are faulted in, written, and read back.
 *
 * That matters because the observed corruption is heap *pointers arriving as
 * zero*: dpkg-deb's decompressor faults at addresses like 0x8001 and 0x38c395,
 * which are dictionary offsets with the base missing, and the kernel reports
 * those addresses as belonging to no VMA at all. A pointer field reading back
 * as zero is what a lost page looks like from userspace -- the page came back
 * zero-filled rather than with the bytes that were written to it.
 *
 * So this checks two things at once, because either could produce that:
 *
 *   memory  each thread owns a multi-megabyte region filled with a pattern
 *           derived from its address, and re-verifies it after every round of
 *           churn. A page that silently reverts to zero, or that aliases
 *           another thread's page, is caught with its address and both values.
 *
 *   registers  x19-x28 and v8-v15 are callee-saved, so a thread's copies must
 *           survive every fault and every switch to another thread of the same
 *           mm. These are verified in the same loop, because if the corruption
 *           is really a lost register rather than a lost page, the workload
 *           would look identical from outside.
 *
 * The churn is deliberate: mapping and unmapping fresh regions between verifies
 * forces the guest to recycle page frames, which is what makes a page-loss bug
 * probabilistic in proportion to volume rather than immediate.
 *
 * Raw clone(2) and raw mmap, no libc, same as the other probes: the point is to
 * pin the kernel's behaviour, not any libc's.
 *
 * Build:
 *   clang --target=aarch64-linux-gnu -static -nostdlib -ffreestanding -O1 \
 *         -fno-stack-protector -o mtmem mtmem.c
 */

typedef unsigned long ulong;
typedef long slong;

#define SYS_write	64
#define SYS_exit	93
#define SYS_exit_group	94
#define SYS_gettid	178
#define SYS_clone	220
#define SYS_mmap	222
#define SYS_munmap	215
#define SYS_sched_yield	124
#define SYS_openat	56

#define CLONE_VM		0x00000100
#define CLONE_FS		0x00000200
#define CLONE_FILES		0x00000400
#define CLONE_SIGHAND		0x00000800
#define CLONE_THREAD		0x00010000
#define CLONE_SETTLS		0x00080000

#define PROT_READ	0x1
#define PROT_WRITE	0x2
#define MAP_PRIVATE	0x02
#define MAP_ANONYMOUS	0x20

#define NTHREADS	6
#define STACK_SIZE	(128 * 1024)
#define REGION_MB	6
#define REGION		(REGION_MB * 1024 * 1024)
#define ROUNDS		12
#define CHURN		24
#define CHURN_SZ	(1024 * 1024)
#define VBYTES		(8 * 16)		/* v8..v15 */

static long sys(long nr, long a, long b, long c, long d, long e, long f)
{
	register long x0 __asm__("x0") = a;
	register long x1 __asm__("x1") = b;
	register long x2 __asm__("x2") = c;
	register long x3 __asm__("x3") = d;
	register long x4 __asm__("x4") = e;
	register long x5 __asm__("x5") = f;
	register long x8 __asm__("x8") = nr;

	__asm__ volatile ("svc #0"
			  : "+r" (x0)
			  : "r" (x1), "r" (x2), "r" (x3), "r" (x4), "r" (x5),
			    "r" (x8)
			  : "memory", "cc");
	return x0;
}

#define sys0(n)			sys((n), 0, 0, 0, 0, 0, 0)
#define sys1(n, a)		sys((n), (long)(a), 0, 0, 0, 0, 0)
#define sys2(n, a, b)		sys((n), (long)(a), (long)(b), 0, 0, 0, 0)
#define sys3(n, a, b, c)	sys((n), (long)(a), (long)(b), (long)(c), 0, 0, 0)

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
	char b[19];
	int i;

	b[0] = '0';
	b[1] = 'x';
	for (i = 0; i < 16; i++)
		b[2 + i] = d[(v >> ((15 - i) * 4)) & 0xf];
	b[18] = '\0';
	out(b);
}

static void outdec(slong v)
{
	char b[24];
	int i = 22;

	b[23] = '\0';
	if (!v) {
		out("0");
		return;
	}
	if (v < 0) {
		out("-");
		v = -v;
	}
	while (v) {
		b[i--] = '0' + (v % 10);
		v /= 10;
	}
	out(&b[i + 1]);
}

/*
 * The pattern is a function of the *address*, not of the offset within the
 * region. That is what makes aliasing visible: if two threads' regions were
 * ever backed by the same page frame, the value read back would be the other
 * address's pattern rather than a zero or a random word, and the report below
 * says which.
 */
static ulong pattern(ulong addr, ulong salt)
{
	ulong x = addr ^ (salt * 0x9e3779b97f4a7c15UL);

	x ^= x >> 33;
	x *= 0xff51afd7ed558ccdUL;
	x ^= x >> 33;
	x *= 0xc4ceb9fe1a85ec53UL;
	x ^= x >> 33;
	return x;
}

struct tinfo {
	ulong tid_slot;
	volatile int done;
	volatile int failed;
	ulong base;
	ulong salt;
	/* first failure, recorded for the report */
	ulong bad_addr;
	ulong bad_want;
	ulong bad_got;
	const char *what;
	unsigned char fp_want[VBYTES] __attribute__((aligned(16)));
	unsigned char fp_seen[VBYTES] __attribute__((aligned(16)));
	ulong gp_want[10];
	ulong gp_seen[10];
};

static struct tinfo tinfo[NTHREADS];

static void vregs_store(void *p)
{
	__asm__ volatile (
		"st1 {v8.16b - v11.16b},  [%0], #64\n"
		"st1 {v12.16b - v15.16b}, [%0], #64\n"
		: "+r" (p) :: "memory");
}

static void vregs_load(const void *p)
{
	__asm__ volatile (
		"ld1 {v8.16b - v11.16b},  [%0], #64\n"
		"ld1 {v12.16b - v15.16b}, [%0], #64\n"
		: "+r" (p) :: "memory",
		  "v8", "v9", "v10", "v11", "v12", "v13", "v14", "v15");
}

/* x19-x28 are callee-saved; the compiler must not be holding anything in them
 * across this, hence the explicit clobber list on the loader. */
static void gpregs_load(const ulong *p)
{
	__asm__ volatile (
		"ldp x19, x20, [%0, #0]\n"
		"ldp x21, x22, [%0, #16]\n"
		"ldp x23, x24, [%0, #32]\n"
		"ldp x25, x26, [%0, #48]\n"
		"ldp x27, x28, [%0, #64]\n"
		:: "r" (p)
		: "x19", "x20", "x21", "x22", "x23", "x24", "x25", "x26",
		  "x27", "x28", "memory");
}

static void gpregs_store(ulong *p)
{
	__asm__ volatile (
		"stp x19, x20, [%0, #0]\n"
		"stp x21, x22, [%0, #16]\n"
		"stp x23, x24, [%0, #32]\n"
		"stp x25, x26, [%0, #48]\n"
		"stp x27, x28, [%0, #64]\n"
		:: "r" (p) : "memory");
}

static void fill_bytes(unsigned char *buf, int n, ulong seed)
{
	ulong x = seed | 1;
	int i;

	for (i = 0; i < n; i++) {
		x ^= x << 13;
		x ^= x >> 7;
		x ^= x << 17;
		buf[i] = (unsigned char)(x ^ (i * 31));
	}
}

static ulong do_mmap(ulong len)
{
	return (ulong)sys(SYS_mmap, 0, (long)len, PROT_READ | PROT_WRITE,
			  MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
}

static void fill_region(struct tinfo *t)
{
	ulong *p = (ulong *)t->base;
	ulong i;

	for (i = 0; i < REGION / sizeof(ulong); i++)
		p[i] = pattern((ulong)&p[i], t->salt);
}

static int verify_region(struct tinfo *t)
{
	ulong *p = (ulong *)t->base;
	ulong i;

	for (i = 0; i < REGION / sizeof(ulong); i++) {
		ulong want = pattern((ulong)&p[i], t->salt);

		if (p[i] != want) {
			t->bad_addr = (ulong)&p[i];
			t->bad_want = want;
			t->bad_got = p[i];
			return 1;
		}
	}
	return 0;
}

static void thread_body(struct tinfo *t)
{
	int r, c;

	fill_bytes(t->fp_want, VBYTES, t->salt * 7 + 3);
	for (r = 0; r < 10; r++)
		t->gp_want[r] = pattern(t->salt * 131 + r, 0x51ee);

	fill_region(t);
	vregs_load(t->fp_want);
	gpregs_load(t->gp_want);

	for (r = 0; r < ROUNDS; r++) {
		/*
		 * Churn first, then verify: recycling frames is what gives a
		 * lost-page bug the opportunity the real workload gives it.
		 */
		for (c = 0; c < CHURN; c++) {
			ulong q = do_mmap(CHURN_SZ);
			ulong j;

			if ((slong)q < 0 && (slong)q > -4096)
				continue;
			for (j = 0; j < CHURN_SZ; j += 4096)
				*(volatile ulong *)(q + j) = q + j;
			sys2(SYS_munmap, q, CHURN_SZ);
			sys0(SYS_sched_yield);
		}

		if (verify_region(t)) {
			t->what = "memory";
			t->failed = 1;
			break;
		}

		vregs_store(t->fp_seen);
		for (c = 0; c < VBYTES; c++) {
			if (t->fp_seen[c] != t->fp_want[c]) {
				t->bad_addr = c;
				t->bad_want = t->fp_want[c];
				t->bad_got = t->fp_seen[c];
				t->what = "vreg";
				t->failed = 1;
				break;
			}
		}
		if (t->failed)
			break;

		gpregs_store(t->gp_seen);
		for (c = 0; c < 10; c++) {
			if (t->gp_seen[c] != t->gp_want[c]) {
				t->bad_addr = 19 + c;
				t->bad_want = t->gp_want[c];
				t->bad_got = t->gp_seen[c];
				t->what = "gpreg";
				t->failed = 1;
				break;
			}
		}
		if (t->failed)
			break;
	}

	t->done = 1;
	sys1(SYS_exit, 0);
	for (;;)
		;
}

static slong spawn(struct tinfo *t, void *stack_top)
{
	register ulong x0 __asm__("x0") =
		CLONE_VM | CLONE_FS | CLONE_FILES | CLONE_SIGHAND |
		CLONE_THREAD | CLONE_SETTLS;
	register ulong x1 __asm__("x1") = (ulong)stack_top;
	register ulong x2 __asm__("x2") = 0;
	register ulong x3 __asm__("x3") = (ulong)&t->tid_slot;	/* tls */
	register ulong x4 __asm__("x4") = 0;
	register ulong x8 __asm__("x8") = SYS_clone;
	register ulong xfn __asm__("x9") = (ulong)thread_body;
	register ulong xarg __asm__("x10") = (ulong)t;

	__asm__ volatile (
		"svc #0\n"
		"cbnz x0, 1f\n"
		"mov x0, %[arg]\n"
		"blr %[fn]\n"
		"mov x8, #93\n"
		"mov x0, #1\n"
		"svc #0\n"
		"1:\n"
		: "+r" (x0)
		: "r" (x1), "r" (x2), "r" (x3), "r" (x4), "r" (x8),
		  [fn] "r" (xfn), [arg] "r" (xarg)
		: "memory", "cc", "x5", "x6", "x7", "x11", "x12", "x13", "x14",
		  "x15", "x16", "x17", "x18", "x19", "x20", "x21", "x22", "x23",
		  "x24", "x25", "x26", "x27", "x28", "x30");

	return (slong)x0;
}

void _start(void);

void _start(void)
{
	int i, fails = 0, started = 0;
	slong r;

	r = sys3(SYS_openat, -100, "/dev/console", 1);
	if (r >= 0)
		outfd = (int)r;

	out("MTMEM_START threads=");
	outdec(NTHREADS);
	out(" region_mb=");
	outdec(REGION_MB);
	out(" rounds=");
	outdec(ROUNDS);
	out("\n");

	for (i = 0; i < NTHREADS; i++) {
		ulong stack = do_mmap(STACK_SIZE);
		ulong base = do_mmap(REGION);

		if ((slong)stack < 0 || (slong)base < 0) {
			out("MTMEM_FAIL mmap\n");
			sys1(SYS_exit_group, 1);
		}
		tinfo[i].base = base;
		tinfo[i].salt = 0x1000 + i;
		/* 16-byte aligned stack top, as AAPCS64 requires. */
		if (spawn(&tinfo[i], (void *)((stack + STACK_SIZE) & ~15UL)) > 0)
			started++;
	}

	for (;;) {
		int done = 0;

		for (i = 0; i < NTHREADS; i++)
			done += tinfo[i].done;
		if (done >= started)
			break;
		sys0(SYS_sched_yield);
	}

	for (i = 0; i < NTHREADS; i++) {
		if (!tinfo[i].failed)
			continue;
		fails++;
		out("MTMEM_FAIL thread=");
		outdec(i);
		out(" what=");
		out(tinfo[i].what ? tinfo[i].what : "?");
		out(" at=");
		outhex(tinfo[i].bad_addr);
		out(" want=");
		outhex(tinfo[i].bad_want);
		out(" got=");
		outhex(tinfo[i].bad_got);
		/*
		 * If the value read back is another thread's pattern for the
		 * same address, the page was aliased rather than lost; say so,
		 * because the two have completely different causes.
		 */
		if (tinfo[i].what[0] == 'm') {
			int j;

			if (!tinfo[i].bad_got)
				out(" (zero-filled)");
			for (j = 0; j < NTHREADS; j++)
				if (j != i &&
				    pattern(tinfo[i].bad_addr, tinfo[j].salt) ==
				    tinfo[i].bad_got) {
					out(" (aliased thread ");
					outdec(j);
					out(")");
				}
		}
		out("\n");
	}

	out("MTMEM_STARTED=");
	outdec(started);
	out(" fails=");
	outdec(fails);
	out("\n");
	out(fails ? "MTMEM_FAILED\n" : "MTMEM_OK\n");
	sys1(SYS_exit_group, fails ? 1 : 0);
	for (;;)
		;
}
