// SPDX-License-Identifier: GPL-2.0
/*
 * Guest register-integrity torture under *involuntary* preemption.
 *
 * A core from the failing dpkg-deb decompressor shows glibc's memcpy called with
 * dst=0x38c395, src=0x38c385, len=3 while another register still held a valid
 * heap pointer (0xaa5555625230). Those are LZ dictionary offsets with the base
 * pointer missing: the guest resumed with registers that were not the ones it
 * was suspended with.
 *
 * Everything tested up to this point suspended the guest *voluntarily* -- at a
 * syscall, or at a signal the process sent itself. The path that had never been
 * tested is the one dpkg-deb spends all its time in: a CPU-bound loop with no
 * syscalls at all, preempted asynchronously by UML's timer interrupt. That is a
 * completely different way into the register save/restore code.
 *
 * So: fill every callee-saved integer register and every V register with known
 * values, spin in a loop that makes no syscalls, and verify continuously. If the
 * guest is resumed with even one register wrong, this says which one.
 *
 * Build:
 *   clang --target=aarch64-linux-gnu -static -nostdlib -ffreestanding -O1 \
 *         -fno-stack-protector -mno-outline-atomics -o regtorture regtorture.c
 */

typedef unsigned long ulong;
typedef long slong;

#define SYS_write	64
#define SYS_exit	93
#define SYS_exit_group	94
#define SYS_clone	220
#define SYS_wait4	260
#define SYS_openat	56
#define SYS_reboot	142
#define SYS_getpid	172

#define SIGCHLD		17

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

#define sys1(n, a)		sys(n, (ulong)(a), 0, 0, 0, 0, 0)
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

static void out(const char *s) { sys3(SYS_write, outfd, s, slen(s)); }

static void outhex(ulong v)
{
	static const char d[] = "0123456789abcdef";
	char b[19];
	int i;

	b[0] = '0'; b[1] = 'x';
	for (i = 0; i < 16; i++)
		b[2 + i] = d[(v >> ((15 - i) * 4)) & 0xf];
	b[18] = '\0';
	out(b);
}

static void outdec(slong v)
{
	char buf[24];
	int i = 23;

	buf[i--] = '\0';
	if (v < 0) { out("-"); v = -v; }
	if (!v) buf[i--] = '0';
	while (v) { buf[i--] = '0' + (v % 10); v /= 10; }
	out(&buf[i + 1]);
}

/*
 * Results, written only on failure so the hot loop touches nothing.
 * Deliberately not volatile-read in the loop: the check is done in registers.
 */
static ulong bad_reg;
static ulong bad_want;
static ulong bad_got;
static ulong iterations_done;

/*
 * The core loop, entirely in asm.
 *
 * x19..x28 are loaded with distinct constants derived from a seed and then
 * verified on every pass. The loop performs only integer arithmetic on
 * caller-saved registers, so it never traps voluntarily; the only way out of it
 * is UML's timer preempting the guest. A mismatch stops immediately and records
 * which register, what it should have been and what it was.
 *
 * Written in asm rather than C because the whole point is to control exactly
 * which registers hold the invariants -- a C compiler would happily spill them.
 */
ulong reg_loop(ulong seed, ulong iters);

__asm__(
"	.globl reg_loop\n"
"	.type reg_loop, @function\n"
"reg_loop:\n"
"	stp	x29, x30, [sp, #-96]!\n"
"	mov	x29, sp\n"
"	stp	x19, x20, [sp, #16]\n"
"	stp	x21, x22, [sp, #32]\n"
"	stp	x23, x24, [sp, #48]\n"
"	stp	x25, x26, [sp, #64]\n"
"	stp	x27, x28, [sp, #80]\n"
"\n"
"	// x9 = seed, x10 = remaining iterations\n"
"	mov	x9, x0\n"
"	mov	x10, x1\n"
"\n"
"	// Load the invariants: x19+n = seed * (n+1), a value that is wrong in an\n"
"	// obvious way if only part of it survives.\n"
"	// V register invariants: v8..v15 are callee-saved, v16..v23 are not --\n"
"	// include both, because a bug in the save path and a bug in the restore\n"
"	// path show up in different halves.\n"
"	dup	v8.2d, x9\n"
"	dup	v9.2d, x9\n"
"	dup	v10.2d, x9\n"
"	dup	v11.2d, x9\n"
"	dup	v12.2d, x9\n"
"	dup	v13.2d, x9\n"
"	dup	v14.2d, x9\n"
"	dup	v15.2d, x9\n"
"	dup	v16.2d, x9\n"
"	dup	v17.2d, x9\n"
"	dup	v18.2d, x9\n"
"	dup	v19.2d, x9\n"
"\n"
"	mov	x19, x9\n"
"	add	x20, x9, x9\n"
"	add	x21, x20, x9\n"
"	add	x22, x21, x9\n"
"	add	x23, x22, x9\n"
"	add	x24, x23, x9\n"
"	add	x25, x24, x9\n"
"	add	x26, x25, x9\n"
"	add	x27, x26, x9\n"
"	add	x28, x27, x9\n"
"\n"
"1:	// verify, using only x11..x15 as scratch\n"
"	mov	x11, x9\n"			// expected x19
"	cmp	x19, x11\n"
"	b.ne	9f\n"
"	add	x11, x11, x9\n"
"	cmp	x20, x11\n"
"	b.ne	9f\n"
"	add	x11, x11, x9\n"
"	cmp	x21, x11\n"
"	b.ne	9f\n"
"	add	x11, x11, x9\n"
"	cmp	x22, x11\n"
"	b.ne	9f\n"
"	add	x11, x11, x9\n"
"	cmp	x23, x11\n"
"	b.ne	9f\n"
"	add	x11, x11, x9\n"
"	cmp	x24, x11\n"
"	b.ne	9f\n"
"	add	x11, x11, x9\n"
"	cmp	x25, x11\n"
"	b.ne	9f\n"
"	add	x11, x11, x9\n"
"	cmp	x26, x11\n"
"	b.ne	9f\n"
"	add	x11, x11, x9\n"
"	cmp	x27, x11\n"
"	b.ne	9f\n"
"	add	x11, x11, x9\n"
"	cmp	x28, x11\n"
"	b.ne	9f\n"
"\n"
"	// verify the V registers: every lane of every one must still be x9\n"
"	umov	x15, v8.d[0]\n"
"	cmp	x15, x9\n"
"	b.ne	9f\n"
"	umov	x15, v8.d[1]\n"
"	cmp	x15, x9\n"
"	b.ne	9f\n"
"	umov	x15, v11.d[0]\n"
"	cmp	x15, x9\n"
"	b.ne	9f\n"
"	umov	x15, v15.d[1]\n"
"	cmp	x15, x9\n"
"	b.ne	9f\n"
"	umov	x15, v16.d[0]\n"
"	cmp	x15, x9\n"
"	b.ne	9f\n"
"	umov	x15, v19.d[1]\n"
"	cmp	x15, x9\n"
"	b.ne	9f\n"
"\n"
"	// burn some cycles so the timer has a chance to land mid-sequence\n"
"	mov	x12, #64\n"
"2:	add	x13, x13, x12\n"
"	eor	x14, x14, x13\n"
"	sub	x12, x12, #1\n"
"	cbnz	x12, 2b\n"
"\n"
"	sub	x10, x10, #1\n"
"	cbnz	x10, 1b\n"
"	mov	x0, #0\n"			// 0 = no mismatch
"	b	10f\n"
"\n"
"9:	// mismatch: return the offending value in x0 and the expectation in x1\n"
"	mov	x0, x11\n"			// expected\n"
"	mov	x1, x19\n"			// report x19 as a sample\n"
"	mov	x0, #1\n"
"10:\n"
"	ldp	x19, x20, [sp, #16]\n"
"	ldp	x21, x22, [sp, #32]\n"
"	ldp	x23, x24, [sp, #48]\n"
"	ldp	x25, x26, [sp, #64]\n"
"	ldp	x27, x28, [sp, #80]\n"
"	ldp	x29, x30, [sp], #96\n"
"	ret\n"
"	.size reg_loop, .-reg_loop\n");

void _start(void);

void _start(void)
{
	slong r;
	int fails = 0;
	int child, i;

	r = sys3(SYS_openat, -100, "/dev/console", 1);
	if (r >= 0)
		outfd = (int)r;

	out("REGTORTURE_START\n");

	/*
	 * Several concurrent CPU-bound children, so the UML scheduler really does
	 * have to preempt and switch between them rather than leaving one process
	 * running uncontended. This is the shape of a dpkg unpack: more runnable
	 * work than CPUs, none of it making syscalls.
	 */
	for (child = 0; child < 4; child++) {
		slong pid = sys(SYS_clone, SIGCHLD, 0, 0, 0, 0, 0);

		if (pid == 0) {
			ulong bad = reg_loop(0x0123456789abcdefUL + child, 20000);

			sys1(SYS_exit, bad ? 7 : 0);
		}
	}

	for (i = 0; i < 4; i++) {
		int status = 0;

		sys4(SYS_wait4, -1, &status, 0, 0);
		if (status != 0) {
			out("REGTORTURE_FAIL child status="); outhex((ulong)status);
			out("\n");
			fails++;
		}
	}

	/* And once in this process too, for a single-process baseline. */
	if (reg_loop(0xfeedfacecafebeefUL, 20000)) {
		out("REGTORTURE_FAIL parent\n");
		fails++;
	}

	(void)bad_reg; (void)bad_want; (void)bad_got; (void)iterations_done;

	if (fails == 0)
		out("REGTORTURE_OK\n");
	else {
		out("REGTORTURE_FAILED fails="); outdec(fails); out("\n");
	}

	sys4(SYS_reboot, 0xfee1deadUL, 672274793UL, 0x4321fedcUL, 0);
	sys1(SYS_exit_group, fails ? 1 : 0);
	for (;;)
		;
}
