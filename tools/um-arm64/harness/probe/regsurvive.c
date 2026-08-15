// SPDX-License-Identifier: GPL-2.0
/*
 * Does a general-purpose register survive a page fault and a thread switch?
 *
 * This exists because of what the gate-7 disassembly says. liblzma faults at
 *
 *	1aaf8: strb w2, [x7, x0]
 *
 * and x7 is written nowhere in the surrounding 0x400 bytes: it is the LZ
 * dictionary base, loaded once and held in a register for the whole decode
 * loop. The faulting addresses are small -- 0x8001, 0x128ff0, 0x3d1ff0 -- and
 * the guest kernel reports them as belonging to no VMA. Those are dictionary
 * *offsets*, so the base in x7 has become zero while the loop was running.
 *
 * Nothing in that loop calls a function, so the compiler's caller/callee-saved
 * split is irrelevant: the only things that can touch x7 are a page fault and an
 * involuntary preemption, both of which take the register set out through UML
 * and back. Every existing probe checks x19-x28 and v8-v15 -- the registers a
 * *compiler* must preserve -- which is precisely the set this failure is not in.
 *
 * So the loop below is written in assembly and holds a known value in all 24
 * registers a syscall must preserve (x1-x7, x9-x15, x19-x28), while doing the
 * two things the real workload does between uses of x7:
 *
 *	- writing to a page it has not touched before, taking a fault that UML
 *	  services by mapping the page into the stub;
 *	- a sched_yield, which hands the shared stub process to another thread
 *	  of the same mm and forces a full register swap.
 *
 * arm64's syscall ABI preserves x1-x30; only x0 carries the result. So every
 * register checked here must come back unchanged, and any that does not is a
 * UML bug rather than a test artefact.
 *
 * Checking is an XOR fold in the fast path -- 24 compares per iteration would
 * dominate the loop -- with a full register dump taken only once something has
 * already gone wrong, so the report can name the register.
 *
 * Build:
 *   clang --target=aarch64-linux-gnu -static -nostdlib -ffreestanding -O1 \
 *         -fno-stack-protector -o regsurvive regsurvive.c
 */

typedef unsigned long ulong;
typedef long slong;

#define SYS_write	64
#define SYS_exit	93
#define SYS_exit_group	94
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

#define NTHREADS	4
#define STACK_SIZE	(128 * 1024)
#define PAGES		512			/* fresh pages touched per pass */
#define PASSES		8
#define NREG		24

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
 * Laid out for the assembly below, which addresses these by byte offset from
 * x17. Keep the order and the offsets in the asm in step.
 */
struct rt {
	ulong ptr;		/* +0   next page to touch */
	ulong end;		/* +8   wrap point */
	ulong base;		/* +16  region start */
	ulong xor_want;		/* +24  expected fold of the 24 registers */
	ulong count;		/* +32  iterations remaining */
	ulong failed;		/* +40 */
	ulong seed;		/* +48  register r should hold seed + r */
	ulong dump[NREG];	/* +56  filled in only after a mismatch */
};

struct tinfo {
	struct rt rt;
	volatile int done;
	ulong salt;
};

static struct tinfo tinfo[NTHREADS];

/* Register numbers in the order the assembly folds and dumps them. */
static const unsigned char regno[NREG] = {
	1, 2, 3, 4, 5, 6, 7,
	9, 10, 11, 12, 13, 14, 15,
	19, 20, 21, 22, 23, 24, 25, 26, 27, 28
};

/*
 * The loop. x17 holds the struct pointer and x16 is scratch; both are IP0/IP1,
 * which the ABI reserves for exactly this and which are therefore not part of
 * what is being tested. x0 and x8 carry the syscall.
 */
static void reg_loop(struct rt *rt)
{
	__asm__ volatile (
		"mov  x17, %0\n"
		/* seed the 24 registers under test: xN = seed + N */
		"ldr  x16, [x17, #48]\n"
		"add  x1,  x16, #1\n"
		"add  x2,  x16, #2\n"
		"add  x3,  x16, #3\n"
		"add  x4,  x16, #4\n"
		"add  x5,  x16, #5\n"
		"add  x6,  x16, #6\n"
		"add  x7,  x16, #7\n"
		"add  x9,  x16, #9\n"
		"add  x10, x16, #10\n"
		"add  x11, x16, #11\n"
		"add  x12, x16, #12\n"
		"add  x13, x16, #13\n"
		"add  x14, x16, #14\n"
		"add  x15, x16, #15\n"
		"add  x19, x16, #19\n"
		"add  x20, x16, #20\n"
		"add  x21, x16, #21\n"
		"add  x22, x16, #22\n"
		"add  x23, x16, #23\n"
		"add  x24, x16, #24\n"
		"add  x25, x16, #25\n"
		"add  x26, x16, #26\n"
		"add  x27, x16, #27\n"
		"add  x28, x16, #28\n"
		"1:\n"
		/* touch the next page; wrap at the end of the region */
		"ldr  x16, [x17, #0]\n"
		"str  x16, [x16]\n"
		"add  x16, x16, #4096\n"
		"ldr  x0,  [x17, #8]\n"
		"cmp  x16, x0\n"
		"b.lo 2f\n"
		"ldr  x16, [x17, #16]\n"
		"2:\n"
		"str  x16, [x17, #0]\n"
		/* hand the stub to another thread of this mm */
		"mov  x8, #124\n"
		"svc  #0\n"
		/* fold the registers under test and compare */
		"eor  x16, x1, x2\n"
		"eor  x16, x16, x3\n"
		"eor  x16, x16, x4\n"
		"eor  x16, x16, x5\n"
		"eor  x16, x16, x6\n"
		"eor  x16, x16, x7\n"
		"eor  x16, x16, x9\n"
		"eor  x16, x16, x10\n"
		"eor  x16, x16, x11\n"
		"eor  x16, x16, x12\n"
		"eor  x16, x16, x13\n"
		"eor  x16, x16, x14\n"
		"eor  x16, x16, x15\n"
		"eor  x16, x16, x19\n"
		"eor  x16, x16, x20\n"
		"eor  x16, x16, x21\n"
		"eor  x16, x16, x22\n"
		"eor  x16, x16, x23\n"
		"eor  x16, x16, x24\n"
		"eor  x16, x16, x25\n"
		"eor  x16, x16, x26\n"
		"eor  x16, x16, x27\n"
		"eor  x16, x16, x28\n"
		"ldr  x0, [x17, #24]\n"
		"cmp  x16, x0\n"
		"b.ne 3f\n"
		/* iterate */
		"ldr  x0, [x17, #32]\n"
		"subs x0, x0, #1\n"
		"str  x0, [x17, #32]\n"
		"b.ne 1b\n"
		"b    4f\n"
		"3:\n"
		/* something moved: record everything, then stop */
		"stp  x1,  x2,  [x17, #56]\n"
		"stp  x3,  x4,  [x17, #72]\n"
		"stp  x5,  x6,  [x17, #88]\n"
		"stp  x7,  x9,  [x17, #104]\n"
		"stp  x10, x11, [x17, #120]\n"
		"stp  x12, x13, [x17, #136]\n"
		"stp  x14, x15, [x17, #152]\n"
		"stp  x19, x20, [x17, #168]\n"
		"stp  x21, x22, [x17, #184]\n"
		"stp  x23, x24, [x17, #200]\n"
		"stp  x25, x26, [x17, #216]\n"
		"stp  x27, x28, [x17, #232]\n"
		"mov  x0, #1\n"
		"str  x0, [x17, #40]\n"
		"4:\n"
		:: "r" (rt)
		: "memory", "cc", "x0", "x1", "x2", "x3", "x4", "x5", "x6",
		  "x7", "x8", "x9", "x10", "x11", "x12", "x13", "x14", "x15",
		  "x16", "x17", "x19", "x20", "x21", "x22", "x23", "x24",
		  "x25", "x26", "x27", "x28");
}

static ulong do_mmap(ulong len)
{
	return (ulong)sys(SYS_mmap, 0, (long)len, PROT_READ | PROT_WRITE,
			  MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
}

static void thread_body(struct tinfo *t)
{
	reg_loop(&t->rt);
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
	register ulong x3 __asm__("x3") = (ulong)&t->salt;
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
	int i, j, started = 0, fails = 0;
	slong r;

	r = sys3(SYS_openat, -100, "/dev/console", 1);
	if (r >= 0)
		outfd = (int)r;

	out("REGSURVIVE_START threads=");
	outdec(NTHREADS);
	out(" pages=");
	outdec(PAGES);
	out(" iters=");
	outdec((slong)PAGES * PASSES);
	out("\n");

	for (i = 0; i < NTHREADS; i++) {
		struct rt *rt = &tinfo[i].rt;
		ulong stack = do_mmap(STACK_SIZE);
		ulong region = do_mmap((ulong)PAGES * 4096);
		ulong x;

		if ((slong)stack < 0 || (slong)region < 0) {
			out("REGSURVIVE_FAIL mmap\n");
			sys1(SYS_exit_group, 1);
		}
		tinfo[i].salt = 0x2000 + i;
		rt->base = region;
		rt->ptr = region;
		rt->end = region + (ulong)PAGES * 4096;
		rt->seed = 0x5a5a000000000000UL + ((ulong)i << 32);
		rt->count = (ulong)PAGES * PASSES;

		x = 0;
		for (j = 0; j < NREG; j++)
			x ^= rt->seed + regno[j];
		rt->xor_want = x;

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
		struct rt *rt = &tinfo[i].rt;

		if (!rt->failed)
			continue;
		fails++;
		out("REGSURVIVE_FAIL thread=");
		outdec(i);
		out(" after=");
		outdec((slong)((ulong)PAGES * PASSES - rt->count));
		out("\n");
		for (j = 0; j < NREG; j++) {
			ulong want = rt->seed + regno[j];

			if (rt->dump[j] == want)
				continue;
			out("  x");
			outdec(regno[j]);
			out(" want=");
			outhex(want);
			out(" got=");
			outhex(rt->dump[j]);
			out("\n");
		}
	}

	out("REGSURVIVE started=");
	outdec(started);
	out(" fails=");
	outdec(fails);
	out("\n");
	out(fails ? "REGSURVIVE_FAILED\n" : "REGSURVIVE_OK\n");
	sys1(SYS_exit_group, fails ? 1 : 0);
	for (;;)
		;
}
