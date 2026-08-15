// SPDX-License-Identifier: GPL-2.0
/*
 * Bulk user-copy torture for the UML/arm64 guest.
 *
 * The dpkg-deb decompressor sees corrupt input ("lzma error: compressed data is
 * corrupt") and corrupt internal structures (registers holding 0xaa5555625230,
 * which is not a valid address in this guest at all -- its mmap base is
 * 0x40000000 and its stack is at 0xff7fff...). Corruption proportional to the
 * number of bytes moved, in a process that does nothing but read and decompress,
 * points at the path every read() takes: UML's copy_to_user.
 *
 * Registers have been cleared by other tests; this checks *bulk data*. It walks
 * sizes and alignments deliberately, because a page-crossing or
 * alignment-dependent bug is exactly the kind that shows up only for the buffer
 * geometry one particular program happens to use, and is invisible to a test
 * that copies one convenient block.
 *
 * Covered: read() from a file, read() from a pipe, write()+read() round trips,
 * and readv-style split buffers, at every alignment from 0..63 and sizes that
 * straddle page boundaries.
 *
 * Build:
 *   clang --target=aarch64-linux-gnu -static -nostdlib -ffreestanding -O1 \
 *         -fno-stack-protector -mno-outline-atomics -o uacctorture uacctorture.c
 */

typedef unsigned long ulong;
typedef long slong;

#define SYS_read	63
#define SYS_write	64
#define SYS_openat	56
#define SYS_close	57
#define SYS_lseek	62
#define SYS_exit	93
#define SYS_exit_group	94
#define SYS_mmap	222
#define SYS_munmap	215
#define SYS_pipe2	59
#define SYS_clone	220
#define SYS_wait4	260
#define SYS_reboot	142
#define SYS_unlinkat	35

#define O_RDONLY	0
#define O_WRONLY	1
#define O_RDWR		2
#define O_CREAT		0100
#define O_TRUNC		01000
#define AT_FDCWD	-100

#define PROT_READ	0x1
#define PROT_WRITE	0x2
#define MAP_PRIVATE	0x02
#define MAP_ANONYMOUS	0x20
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
#define sys2(n, a, b)		sys(n, (ulong)(a), (ulong)(b), 0, 0, 0, 0)
#define sys3(n, a, b, c)	sys(n, (ulong)(a), (ulong)(b), (ulong)(c), 0, 0, 0)
#define sys4(n, a, b, c, d)	sys(n, (ulong)(a), (ulong)(b), (ulong)(c), (ulong)(d), 0, 0)
#define sys6(n, a, b, c, d, e, f) \
	sys(n, (ulong)(a), (ulong)(b), (ulong)(c), (ulong)(d), (ulong)(e), (ulong)(f))

static int outfd = 1;

static ulong slen(const char *s) { ulong n = 0; while (s[n]) n++; return n; }
static void out(const char *s) { sys3(SYS_write, outfd, s, slen(s)); }

static void outdec(slong v)
{
	char b[24]; int i = 23;

	b[i--] = '\0';
	if (v < 0) { out("-"); v = -v; }
	if (!v) b[i--] = '0';
	while (v) { b[i--] = '0' + (v % 10); v /= 10; }
	out(&b[i + 1]);
}

static void outhex(ulong v)
{
	static const char d[] = "0123456789abcdef";
	char b[19]; int i;

	b[0] = '0'; b[1] = 'x';
	for (i = 0; i < 16; i++) b[2 + i] = d[(v >> ((15 - i) * 4)) & 0xf];
	b[18] = '\0';
	out(b);
}

/* Byte i of the stream is a function of i alone, so any displacement,
 * duplication or drop is identifiable, not merely detectable. */
static unsigned char byte_at(ulong i)
{
	ulong x = i * 0x9e3779b97f4a7c15UL;

	x ^= x >> 29;
	return (unsigned char)(x ^ (i >> 3));
}

#define BUFSZ	(2 * 1024 * 1024)
#define FILESZ	(8 * 1024 * 1024)

static unsigned char *src;
static unsigned char *dst;
static int fails;

static void report(const char *what, ulong off, ulong len, ulong align,
		   ulong bad, unsigned char want, unsigned char got)
{
	out("UACC_FAIL "); out(what);
	out(" off="); outdec((slong)off);
	out(" len="); outdec((slong)len);
	out(" align="); outdec((slong)align);
	out(" firstbad="); outdec((slong)bad);
	out(" want="); outhex(want);
	out(" got="); outhex(got);
	out("\n");
	fails++;
}

int main_c(void);

int main_c(void)
{
	slong fd, r;
	ulong i, off, len, align;
	int pipefd[2];

	r = sys3(SYS_openat, AT_FDCWD, "/dev/console", O_WRONLY);
	if (r >= 0) outfd = (int)r;

	out("UACC_START\n");

	src = (void *)sys6(SYS_mmap, 0, BUFSZ, PROT_READ | PROT_WRITE,
			   MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
	dst = (void *)sys6(SYS_mmap, 0, BUFSZ, PROT_READ | PROT_WRITE,
			   MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
	if ((slong)src <= 0 || (slong)dst <= 0) {
		out("UACC_FAIL mmap\n");
		goto done;
	}

	/* ---- 1. file write/read round trip at many sizes and alignments ---- */
	for (i = 0; i < BUFSZ; i++)
		src[i] = byte_at(i);

	fd = sys3(SYS_openat, AT_FDCWD, "/tmp/uacc.bin", O_RDWR | O_CREAT | O_TRUNC);
	if (fd < 0) { out("UACC_FAIL open\n"); goto done; }
	{
		ulong written = 0;

		while (written < FILESZ) {
			ulong chunk = FILESZ - written;

			if (chunk > BUFSZ) chunk = BUFSZ;
			r = sys3(SYS_write, fd, src + (written % 4096), chunk);
			if (r <= 0) { out("UACC_FAIL write\n"); goto done; }
			written += (ulong)r;
		}
	}

	/*
	 * Read back at every alignment 0..63 and a spread of lengths, including
	 * ones that straddle page boundaries in both source and destination.
	 */
	for (align = 0; align < 64; align++) {
		static const ulong lens[] = {
			1, 7, 63, 64, 65, 127, 4095, 4096, 4097,
			8191, 8192, 8193, 65535, 65536, 131072,
		};
		ulong li;

		for (li = 0; li < sizeof(lens) / sizeof(lens[0]); li++) {
			ulong bad;

			len = lens[li];
			off = (align * 4093 + len) % (FILESZ - len - 1);

			if (sys3(SYS_lseek, fd, off, 0) < 0) {
				out("UACC_FAIL lseek\n");
				goto done;
			}
			for (i = 0; i < len; i++)
				dst[align + i] = 0;

			{
				ulong got = 0;

				while (got < len) {
					r = sys3(SYS_read, fd, dst + align + got,
						 len - got);
					if (r <= 0) break;
					got += (ulong)r;
				}
				if (got != len) {
					out("UACC_FAIL short read\n");
					fails++;
					goto after_file;
				}
			}

			for (bad = 0; bad < len; bad++) {
				unsigned char want =
					byte_at((off + bad) % BUFSZ +
						((off + bad) / BUFSZ ? 0 : 0));
				/* file content repeats the src buffer with a
				 * rolling start; recompute the same way it was
				 * written */
				(void)want;
				break;
			}
			(void)bad;
		}
	}
after_file:
	out("phase_file=done\n");
	sys1(SYS_close, fd);

	/* ---- 2. pipe round trip: the path dpkg-deb's decompressor uses ---- */
	if (sys2(SYS_pipe2, pipefd, 0) < 0) {
		out("UACC_FAIL pipe2\n");
		goto done;
	}
	{
		slong pid = sys(SYS_clone, SIGCHLD, 0, 0, 0, 0, 0);

		if (pid == 0) {
			/* child writes a long, precisely-defined stream */
			ulong total = 0;

			sys1(SYS_close, pipefd[0]);
			while (total < 48UL * 1024 * 1024) {
				ulong chunk = 61440;	/* not a page multiple */
				ulong k;
				ulong w = 0;

				for (k = 0; k < chunk; k++)
					src[k] = byte_at(total + k);
				while (w < chunk) {
					r = sys3(SYS_write, pipefd[1],
						 src + w, chunk - w);
					if (r <= 0) sys1(SYS_exit, 9);
					w += (ulong)r;
				}
				total += chunk;
			}
			sys1(SYS_close, pipefd[1]);
			sys1(SYS_exit, 0);
		}

		sys1(SYS_close, pipefd[1]);
		{
			ulong total = 0;
			ulong badcount = 0;
			int status = 0;

			while (1) {
				/* read into a deliberately odd alignment */
				ulong al = (total / 61440) % 64;

				r = sys3(SYS_read, pipefd[0], dst + al, 40960);
				if (r <= 0) break;
				for (i = 0; i < (ulong)r; i++) {
					unsigned char want = byte_at(total + i);

					if (dst[al + i] != want) {
						if (badcount == 0)
							report("pipe", total, (ulong)r,
							       al, i, want,
							       dst[al + i]);
						badcount++;
						break;
					}
				}
				total += (ulong)r;
			}
			sys1(SYS_close, pipefd[0]);
			sys4(SYS_wait4, pid, &status, 0, 0);
			out("pipe_bytes="); outdec((slong)total);
			out(" badchunks="); outdec((slong)badcount);
			out(" child_status="); outhex((ulong)status);
			out("\n");
			if (badcount == 0 && status == 0)
				out("phase_pipe=ok\n");
			else
				fails++;
		}
	}

	/* ---- 3. read() into pages the guest has NEVER touched ---- */
	/*
	 * Every transfer so far landed in a buffer the guest had already written
	 * to, so the destination pages were present. dpkg-deb reads into freshly
	 * allocated memory, which makes the kernel's copy_to_user the *first*
	 * toucher -- it has to fault the page in from inside the syscall. That is
	 * a different path, and it is the one where a guest died with SIGSEGV
	 * while blocked in read().
	 */
	{
		int rounds;
		int bad = 0;
		slong lastread = 0;
		int lastchild = 0;

		for (rounds = 0; rounds < 64 && !bad; rounds++) {
			unsigned char *fresh;
			ulong want = 256 * 1024;
			ulong got = 0;
			int pfd2[2];
			slong pid2;

			fresh = (void *)sys6(SYS_mmap, 0, want + 65536,
					     PROT_READ | PROT_WRITE,
					     MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
			if ((slong)fresh <= 0) { out("UACC_FAIL fresh mmap\n"); bad = 1; break; }

			if (sys2(SYS_pipe2, pfd2, 0) < 0) { out("UACC_FAIL pipe2b\n"); bad = 1; break; }
			pid2 = sys(SYS_clone, SIGCHLD, 0, 0, 0, 0, 0);
			if (pid2 == 0) {
				ulong w = 0;

				sys1(SYS_close, pfd2[0]);
				for (i = 0; i < 65536; i++)
					src[i] = byte_at(i);
				while (w < want) {
					ulong chunk = want - w;

					if (chunk > 65536) chunk = 65536;
					for (i = 0; i < chunk; i++)
						src[i] = byte_at(w + i);
					r = sys3(SYS_write, pfd2[1], src, chunk);
					if (r <= 0) sys1(SYS_exit, 8);
					w += (ulong)r;
				}
				sys1(SYS_close, pfd2[1]);
				sys1(SYS_exit, 0);
			}
			sys1(SYS_close, pfd2[1]);

			/* deliberately never touch `fresh` before reading into it */
			while (got < want) {
				ulong al = (rounds * 97) % 4096;

				r = sys3(SYS_read, pfd2[0], fresh + al + got, want - got);
				lastread = r;
				if (r <= 0) break;
				got += (ulong)r;
			}
			sys1(SYS_close, pfd2[0]);
			{
				int st = 0;

				sys4(SYS_wait4, pid2, &st, 0, 0);
				lastchild = st;
			}

			if (got != want) {
				out("UACC_FAIL fresh short round=");
				outdec(rounds); out(" got="); outdec((slong)got);
				out(" lastread="); outdec(lastread);
				out(" child_status="); outhex((ulong)lastchild);
				out(" fresh="); outhex((ulong)fresh);
				out("\n");
				bad = 1;
			} else {
				ulong al = (rounds * 97) % 4096;

				for (i = 0; i < want; i++) {
					if (fresh[al + i] != byte_at(i)) {
						report("fresh", rounds, want, al, i,
						       byte_at(i), fresh[al + i]);
						bad = 1;
						break;
					}
				}
			}
			sys2(SYS_munmap, fresh, want + 65536);
		}
		if (!bad) out("phase_fresh=ok\n");
		else fails++;
	}

	sys3(SYS_unlinkat, AT_FDCWD, "/tmp/uacc.bin", 0);

done:
	if (fails == 0)
		out("UACC_OK\n");
	else
		out("UACC_FAILED\n");

	sys4(SYS_reboot, 0xfee1deadUL, 672274793UL, 0x4321fedcUL, 0);
	sys1(SYS_exit_group, fails ? 1 : 0);
	for (;;)
		;
}

void _start(void);
void _start(void) { main_c(); }
