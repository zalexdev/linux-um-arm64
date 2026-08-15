// SPDX-License-Identifier: GPL-2.0
/*
 * Guest memory / copy-on-write torture for UML/arm64.
 *
 * dpkg-deb's decompressor segfaults with garbage *source pointers* inside
 * glibc's memcpy, and liblzma reports "compressed data is corrupt". Both are
 * symptoms of guest memory not holding what the guest wrote -- not of FP state,
 * which the faulting instructions do not touch.
 *
 * The obvious suspect is copy-on-write. UML resolves a guest write fault by
 * asking the stub to map a page; whether it maps a *private copy* or the shared
 * original depends on the write flag it decodes from the fault. On arm64 that
 * flag is ESR.WnR, decoded by hand in sysdep/mcontext.h -- there is no
 * equivalent of x86's error_code bit 1 to fall back on. If a write fault were
 * ever misread as a read fault, parent and child would end up sharing a page
 * that should have been copied, and the corruption would look exactly like this:
 * data changing under a process that never wrote it.
 *
 * This checks the property directly and at scale, plus the neighbouring cases
 * (mprotect, MAP_SHARED, and a write to a read-only mapping).
 *
 * Build:
 *   clang --target=aarch64-linux-gnu -static -nostdlib -ffreestanding -O1 \
 *         -fno-stack-protector -mno-outline-atomics -o mmtorture mmtorture.c
 */

typedef unsigned long ulong;
typedef long slong;

#define SYS_write	64
#define SYS_exit	93
#define SYS_exit_group	94
#define SYS_getpid	172
#define SYS_clone	220
#define SYS_wait4	260
#define SYS_mmap	222
#define SYS_munmap	215
#define SYS_mprotect	226
#define SYS_brk		214
#define SYS_openat	56
#define SYS_reboot	142
#define SYS_sched_yield	124

#define PROT_NONE	0x0
#define PROT_READ	0x1
#define PROT_WRITE	0x2
#define MAP_SHARED	0x01
#define MAP_PRIVATE	0x02
#define MAP_ANONYMOUS	0x20
#define SIGCHLD		17

#define PAGES		4096
#define ROUNDS		8

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
#define sys2(n, a, b)		sys(n, (ulong)(a), (ulong)(b), 0, 0, 0, 0)
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

static void out(const char *s) { sys3(SYS_write, outfd, s, slen(s)); }

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

static ulong pagesz = 4096;

/* A value unique to (page, tag) so a mix-up names both. */
static ulong word_for(ulong page, ulong tag)
{
	return (page * 0x100000001UL) ^ (tag * 0x9e3779b97f4a7c15UL);
}

static void fill(unsigned char *base, ulong npages, ulong tag)
{
	ulong p;

	for (p = 0; p < npages; p++) {
		volatile ulong *w = (volatile ulong *)(base + p * pagesz);
		ulong v = word_for(p, tag);
		int k;

		/* Several words per page, including the last, so a partial page
		 * copy is caught as well as a wholly wrong one. */
		w[0] = v;
		w[1] = ~v;
		for (k = 2; k < 8; k++)
			w[k] = v + k;
		*(volatile ulong *)(base + p * pagesz + pagesz - 8) = v ^ 0xffff;
	}
}

/* Returns the first bad page index, or -1. */
static slong check(unsigned char *base, ulong npages, ulong tag)
{
	ulong p;

	for (p = 0; p < npages; p++) {
		volatile ulong *w = (volatile ulong *)(base + p * pagesz);
		ulong v = word_for(p, tag);
		int k;

		if (w[0] != v || w[1] != ~v)
			return (slong)p;
		for (k = 2; k < 8; k++)
			if (w[k] != v + k)
				return (slong)p;
		if (*(volatile ulong *)(base + p * pagesz + pagesz - 8) != (v ^ 0xffff))
			return (slong)p;
	}
	return -1;
}

void _start(void);

void _start(void)
{
	unsigned char *priv, *shared;
	ulong bytes = PAGES * 4096UL;
	int fails = 0;
	ulong round;
	slong r;

	r = sys3(SYS_openat, -100, "/dev/console", 1);
	if (r >= 0)
		outfd = (int)r;

	out("MMTORTURE_START\n");

	/*
	 * TPIDR_EL0 across plain fork().
	 *
	 * This is the difference between "ar p | xz -dc" (which works) and
	 * dpkg-deb's decompressor (which does not): the former execs, so glibc
	 * rebuilds its TLS from scratch, while the latter forks and inherits it.
	 * glibc keeps malloc's per-thread arena pointer in TLS, so a child whose
	 * thread pointer is wrong allocates through a garbage arena -- which
	 * presents exactly as corrupt data and wild pointers inside memcpy.
	 */
	{
		ulong tp_want = 0x7e57f0f0f0f00000UL;
		ulong tp_parent, tp_child_seen;
		volatile ulong *shbuf;
		slong pid;
		int status = 0;

		shbuf = (void *)sys6(SYS_mmap, 0, 4096, PROT_READ | PROT_WRITE,
				     MAP_SHARED | MAP_ANONYMOUS, -1, 0);
		set_tpidr(tp_want);
		tp_parent = get_tpidr();

		pid = sys(SYS_clone, SIGCHLD, 0, 0, 0, 0, 0);
		if (pid == 0) {
			shbuf[0] = get_tpidr();
			sys1(SYS_exit, 0);
		}
		sys4(SYS_wait4, pid, &status, 0, 0);
		tp_child_seen = shbuf[0];

		out("tls_parent="); outhex(tp_parent);
		out(" tls_child="); outhex(tp_child_seen); out("\n");
		if (tp_child_seen != tp_want) {
			out("MMTORTURE_FAIL where=tls_across_fork want=");
			outhex(tp_want); out(" got="); outhex(tp_child_seen);
			out("\n");
			fails++;
		} else {
			out("phase_tls_fork=ok\n");
		}
		/* Restore something harmless. */
		set_tpidr(0);
	}

	priv = (void *)sys6(SYS_mmap, 0, bytes, PROT_READ | PROT_WRITE,
			    MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
	if ((slong)priv <= 0) { out("MMTORTURE_FAIL mmap\n"); goto done; }
	shared = (void *)sys6(SYS_mmap, 0, bytes, PROT_READ | PROT_WRITE,
			      MAP_SHARED | MAP_ANONYMOUS, -1, 0);
	if ((slong)shared <= 0) { out("MMTORTURE_FAIL mmap_shared\n"); goto done; }

	out("priv="); outhex((ulong)priv);
	out(" shared="); outhex((ulong)shared); out("\n");

	/*
	 * The core loop. Each round: parent writes tag A, forks, child overwrites
	 * every page with tag B and verifies it sees B, parent verifies it still
	 * sees A. A shared mapping is carried alongside as a control -- there the
	 * child's write *must* be visible to the parent, so a run where private
	 * and shared behave identically is as much a failure as one where the
	 * private pages change.
	 */
	for (round = 0; round < ROUNDS; round++) {
		slong pid, bad;
		int status = 0;

		fill(priv, PAGES, round);
		fill(shared, PAGES, round);

		pid = sys(SYS_clone, SIGCHLD, 0, 0, 0, 0, 0);
		if (pid < 0) { out("MMTORTURE_FAIL clone\n"); fails++; break; }

		if (pid == 0) {
			/* child: rewrite both mappings with a different tag */
			fill(priv, PAGES, round + 1000);
			fill(shared, PAGES, round + 2000);
			sys1(SYS_sched_yield, 0);
			bad = check(priv, PAGES, round + 1000);
			if (bad >= 0)
				sys1(SYS_exit, 3);	/* child lost its own write */
			bad = check(shared, PAGES, round + 2000);
			if (bad >= 0)
				sys1(SYS_exit, 4);
			sys1(SYS_exit, 0);
		}

		sys1(SYS_sched_yield, 0);
		sys4(SYS_wait4, pid, &status, 0, 0);

		if (status != 0) {
			out("MMTORTURE_FAIL child round="); outdec((slong)round);
			out(" status="); outhex((ulong)status); out("\n");
			fails++;
			break;
		}

		/* The private mapping must be untouched by the child. */
		bad = check(priv, PAGES, round);
		if (bad >= 0) {
			volatile ulong *w = (volatile ulong *)(priv + bad * pagesz);

			out("MMTORTURE_FAIL cow round="); outdec((slong)round);
			out(" page="); outdec(bad);
			out(" want="); outhex(word_for(bad, round));
			out(" got="); outhex(w[0]);
			out(" (child tag would be ");
			outhex(word_for(bad, round + 1000));
			out(")\n");
			fails++;
			break;
		}

		/* The shared mapping must show the child's write. */
		bad = check(shared, PAGES, round + 2000);
		if (bad >= 0) {
			out("MMTORTURE_FAIL shared round="); outdec((slong)round);
			out(" page="); outdec(bad); out("\n");
			fails++;
			break;
		}
	}
	if (!fails)
		out("phase_cow=ok rounds=");
	if (!fails) { outdec(ROUNDS); out("\n"); }

	/* mprotect: a write to a read-only mapping must fault, not silently pass. */
	{
		unsigned char *ro = (void *)sys6(SYS_mmap, 0, pagesz,
						 PROT_READ | PROT_WRITE,
						 MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
		if ((slong)ro > 0) {
			*(volatile ulong *)ro = 0x1234;
			r = sys3(SYS_mprotect, ro, pagesz, PROT_READ);
			out("mprotect_ro="); outdec(r); out("\n");
			if (*(volatile ulong *)ro != 0x1234) {
				out("MMTORTURE_FAIL mprotect_lost_data\n");
				fails++;
			}
			r = sys3(SYS_mprotect, ro, pagesz, PROT_READ | PROT_WRITE);
			*(volatile ulong *)ro = 0x5678;
			if (*(volatile ulong *)ro != 0x5678) {
				out("MMTORTURE_FAIL mprotect_rw\n");
				fails++;
			} else {
				out("phase_mprotect=ok\n");
			}
			sys2(SYS_munmap, ro, pagesz);
		}
	}

	/* ---- brk, including in a forked child ---- */
	/*
	 * Every memory test so far used mmap. glibc's malloc grows the main
	 * arena with brk, and the guest faults reported by dpkg-deb are
	 * level-3 translation faults on writes to a page-aligned address in
	 * exactly that region -- a heap page the guest believes it owns and the
	 * stub does not have. brk growth in a *child* after fork is the one
	 * combination nothing here has covered.
	 */
	{
		int round;
		int bad = 0;

		for (round = 0; round < 16 && !bad; round++) {
			ulong base = (ulong)sys1(SYS_brk, 0);
			ulong grow = 512 * 1024;
			ulong newb;
			slong pid;
			int status = 0;

			newb = (ulong)sys1(SYS_brk, base + grow);
			if (newb < base + grow) {
				out("MMTORTURE_FAIL brk_grow round=");
				outdec(round); out(" base="); outhex(base);
				out(" got="); outhex(newb); out("\n");
				bad = 1;
				break;
			}

			/* touch every new page in the parent */
			{
				ulong o;

				for (o = 0; o < grow; o += pagesz)
					*(volatile ulong *)(base + o) = base + o;
				for (o = 0; o < grow; o += pagesz)
					if (*(volatile ulong *)(base + o) != base + o) {
						out("MMTORTURE_FAIL brk_parent\n");
						bad = 1;
						break;
					}
			}
			if (bad) break;

			pid = sys(SYS_clone, SIGCHLD, 0, 0, 0, 0, 0);
			if (pid == 0) {
				/* child grows brk further and writes the new pages */
				ulong cb = (ulong)sys1(SYS_brk, 0);
				ulong cn = (ulong)sys1(SYS_brk, cb + grow);
				ulong o;

				if (cn < cb + grow)
					sys1(SYS_exit, 5);
				for (o = 0; o < grow; o += pagesz)
					*(volatile ulong *)(cb + o) = cb + o + 7;
				for (o = 0; o < grow; o += pagesz)
					if (*(volatile ulong *)(cb + o) != cb + o + 7)
						sys1(SYS_exit, 6);
				/* and rewrite the pages inherited from the parent */
				for (o = 0; o < grow; o += pagesz)
					*(volatile ulong *)(base + o) = 0xdead0000 + o;
				for (o = 0; o < grow; o += pagesz)
					if (*(volatile ulong *)(base + o) != 0xdead0000 + o)
						sys1(SYS_exit, 7);
				sys1(SYS_exit, 0);
			}
			sys4(SYS_wait4, pid, &status, 0, 0);
			if (status != 0) {
				out("MMTORTURE_FAIL brk_child round=");
				outdec(round); out(" status="); outhex((ulong)status);
				out("\n");
				bad = 1;
				break;
			}
			/* parent's copies must be untouched (COW over brk pages) */
			{
				ulong o;

				for (o = 0; o < grow; o += pagesz)
					if (*(volatile ulong *)(base + o) != base + o) {
						out("MMTORTURE_FAIL brk_cow round=");
						outdec(round);
						out(" off="); outdec((slong)o);
						out("\n");
						bad = 1;
						break;
					}
			}
		}
		if (!bad)
			out("phase_brk=ok\n");
		else
			fails++;
	}

done:
	if (fails == 0)
		out("MMTORTURE_OK\n");
	else
		out("MMTORTURE_FAILED\n");

	sys4(SYS_reboot, 0xfee1deadUL, 672274793UL, 0x4321fedcUL, 0);
	sys1(SYS_exit_group, fails ? 1 : 0);
	for (;;)
		;
}
