// SPDX-License-Identifier: GPL-2.0
/*
 * A freestanding aarch64 init for the UML/arm64 guest.
 *
 * No libc, no dynamic loader, no TLS, no stack protector: its own _start, raw
 * "svc #0" syscalls, and static output. That is the point -- when a guest
 * process dies inside ld-musl or glibc's startup you cannot tell whether the
 * kernel is broken or merely the *first thing* the loader does is broken. This
 * exercises one guest capability per line and prints the result, so the failing
 * one is named rather than inferred.
 *
 * Build:
 *   clang --target=aarch64-linux-gnu -static -nostdlib -ffreestanding -O1 \
 *         -fno-stack-protector -o guestinit guestinit.c
 */

typedef unsigned long ulong;
typedef long slong;

/* arm64 asm-generic syscall numbers. */
#define SYS_write	64
#define SYS_exit	93
#define SYS_exit_group	94
#define SYS_getpid	172
#define SYS_gettid	178
#define SYS_brk		214
#define SYS_munmap	215
#define SYS_mmap	222
#define SYS_clone	220
#define SYS_wait4	260
#define SYS_uname	160
#define SYS_openat	56
#define SYS_read	63
#define SYS_close	57
#define SYS_nanosleep	101
#define SYS_getcwd	17
#define SYS_set_tid_address 96

#define PROT_READ	0x1
#define PROT_WRITE	0x2
#define MAP_PRIVATE	0x02
#define MAP_ANONYMOUS	0x20

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
#define sys6(n, a, b, c, d, e, f) \
	sys(n, (ulong)(a), (ulong)(b), (ulong)(c), (ulong)(d), (ulong)(e), (ulong)(f))

/*
 * Output fd. Init is started with whatever the kernel opened on /dev/console;
 * if the initramfs has no console node, fd 1 is closed and every write silently
 * fails -- which looks exactly like "the guest never ran". Open the console
 * explicitly and fall back to fd 1, so a missing device node is reported rather
 * than mistaken for a kernel bug.
 */
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

	buf[0] = '0';
	buf[1] = 'x';
	for (i = 0; i < 16; i++)
		buf[2 + i] = d[(v >> ((15 - i) * 4)) & 0xf];
	buf[18] = '\0';
	out(buf);
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

static void kv(const char *k, slong v)
{
	out(k);
	out("=");
	outdec(v);
	out("\n");
}

/* Read the whole of a small proc file and echo it, prefixed. */
static void cat(const char *path, const char *tag)
{
	char buf[512];
	slong fd, n;

	fd = sys3(SYS_openat, -100 /* AT_FDCWD */, path, 0);
	if (fd < 0) {
		out(tag);
		out(": open failed ");
		outdec(fd);
		out("\n");
		return;
	}
	n = sys3(SYS_read, fd, buf, sizeof(buf) - 1);
	sys1(SYS_close, fd);
	if (n <= 0) {
		out(tag);
		out(": read failed\n");
		return;
	}
	buf[n] = '\0';
	out(tag);
	out(": ");
	out(buf);
	if (buf[n - 1] != '\n')
		out("\n");
}

/*
 * Capture the initial stack pointer so the auxiliary vector can be walked.
 * At process entry sp points at argc, followed by argv[], NULL, envp[], NULL,
 * then the auxv pairs. There is no portable C way to get at that, hence the
 * asm entry stub.
 */
__asm__(
"	.globl _start\n"
"	.type _start, @function\n"
"_start:\n"
"	mov	x0, sp\n"
"	b	um_start_c\n"
"	.size _start, .-_start\n");

void um_start_c(unsigned long *sp);

#define AT_NULL		0
#define AT_PAGESZ	6
#define AT_BASE		7
#define AT_ENTRY	9
#define AT_UID		11
#define AT_HWCAP	16
#define AT_CLKTCK	17
#define AT_SECURE	23
#define AT_HWCAP2	26
#define AT_SYSINFO_EHDR	33

static void dump_auxv(unsigned long *sp)
{
	unsigned long argc = sp[0];
	unsigned long *p = &sp[1 + argc + 1];	/* skip argv[] and its NULL */
	unsigned long *a;

	while (*p)				/* skip envp[] */
		p++;
	a = p + 1;

	for (; a[0] != AT_NULL; a += 2) {
		const char *name = 0;

		switch (a[0]) {
		case AT_PAGESZ:		name = "AT_PAGESZ"; break;
		case AT_HWCAP:		name = "AT_HWCAP"; break;
		case AT_HWCAP2:		name = "AT_HWCAP2"; break;
		case AT_SYSINFO_EHDR:	name = "AT_SYSINFO_EHDR"; break;
		case AT_ENTRY:		name = "AT_ENTRY"; break;
		case AT_SECURE:		name = "AT_SECURE"; break;
		case AT_CLKTCK:		name = "AT_CLKTCK"; break;
		}
		if (!name)
			continue;
		out(name); out("="); outhex(a[1]); out("\n");
	}
}

void um_start_c(unsigned long *sp)
{
	slong pid, tid, r;
	ulong brk0, brk1;
	void *m;
	volatile unsigned char *p;

	/* O_WRONLY, and AT_FDCWD is -100 */
	r = sys3(SYS_openat, -100, "/dev/console", 1);
	if (r >= 0)
		outfd = (int)r;

	out("UMARM_GUEST_ALIVE\n");
	kv("console_fd", outfd);
	dump_auxv(sp);

	/* --- identity: proves the syscall path and return values work --- */
	pid = sys0(SYS_getpid);
	kv("getpid", pid);
	tid = sys0(SYS_gettid);
	kv("gettid", tid);

	/* --- brk: the simplest form of address-space growth --- */
	brk0 = (ulong)sys1(SYS_brk, 0);
	out("brk0="); outhex(brk0); out("\n");
	brk1 = (ulong)sys1(SYS_brk, brk0 + 0x10000);
	out("brk1="); outhex(brk1); out("\n");
	if (brk1 > brk0) {
		/*
		 * Touch the new break. This is the first *write* to a page the
		 * guest kernel had to fault in, so it exercises the whole
		 * SKAS fault path: stub SIGSEGV -> ESR decode -> page table
		 * update -> mmap in the stub -> resume.
		 */
		p = (volatile unsigned char *)brk0;
		p[0] = 0x5a;
		p[0x1000] = 0x5b;
		out("brk_write=ok val="); outdec(p[0]); out("\n");
	}

	/* --- anonymous mmap: what musl's allocator failed on --- */
	m = (void *)sys6(SYS_mmap, 0, 0x20000, PROT_READ | PROT_WRITE,
			 MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
	out("mmap="); outhex((ulong)m); out("\n");
	if ((slong)m > 0) {
		p = m;
		p[0] = 0x11;
		p[0x1000] = 0x22;
		p[0x1fff0] = 0x33;
		out("mmap_write=ok sum=");
		outdec(p[0] + p[0x1000] + p[0x1fff0]);
		out("\n");
		r = sys2(SYS_munmap, m, 0x20000);
		kv("munmap", r);
	}

	/* --- a second, larger mmap to shake out address-space arithmetic --- */
	m = (void *)sys6(SYS_mmap, 0, 0x400000, PROT_READ | PROT_WRITE,
			 MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
	out("mmap_big="); outhex((ulong)m); out("\n");
	if ((slong)m > 0) {
		p = m;
		p[0x3ffff0] = 0x44;
		out("mmap_big_write=ok\n");
		sys2(SYS_munmap, m, 0x400000);
	}

	/* --- proc, which needs a working VFS and task plumbing --- */
	cat("/proc/self/stat", "stat");
	cat("/proc/uptime", "uptime");

	out("UMARM_GUEST_OK\n");

	/*
	 * Power the machine off rather than just exiting. init exiting is a
	 * kernel panic ("Attempted to kill init!"), and with panic=-1 that is a
	 * fatal signal at the process level -- indistinguishable from a genuine
	 * crash, which blunts the gate's failure signal.
	 *
	 * LINUX_REBOOT_MAGIC1, LINUX_REBOOT_MAGIC2, LINUX_REBOOT_CMD_POWER_OFF.
	 */
	sys(142 /* SYS_reboot */, 0xfee1deadUL, 672274793UL, 0x4321fedcUL, 0, 0, 0);

	sys1(SYS_exit_group, 0);

	for (;;)
		;
}
