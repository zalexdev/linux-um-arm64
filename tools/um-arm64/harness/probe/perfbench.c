// SPDX-License-Identifier: GPL-2.0
/*
 * What does a UML guest actually cost?
 *
 * "UML runs at native speed" is half true and the half that is false is the
 * half people notice. Guest *computation* runs natively -- the instructions
 * execute on the CPU with nothing in between. Guest *kernel entries* do not:
 * every syscall and every page fault has to be intercepted and serviced by the
 * UML kernel, and that interception is the whole cost.
 *
 * So measure the three separately rather than timing "a shell" and guessing:
 *
 *   compute   -- pure userspace arithmetic, no kernel entry at all. This is the
 *                number that should be indistinguishable from native, and if it
 *                is not then something is wrong that no amount of syscall
 *                tuning will fix.
 *   syscall   -- getppid() in a loop. Chosen because it is the cheapest syscall
 *                there is, so the measurement is almost entirely interception
 *                overhead rather than work the kernel does.
 *   fault     -- touching fresh anonymous pages. In ptrace mode each one is a
 *                SIGSEGV to the tracer and a round trip; in SECCOMP mode the
 *                path is shorter. This is what makes program startup slow.
 *   forkexec  -- fork() + execve() + wait(). The compound case, and the one a
 *                shell script spends its life doing.
 *
 * Prints microseconds per operation so the numbers are comparable between a
 * guest and the host it runs on, and between SECCOMP and ptrace mode.
 *
 * Build:
 *   clang --target=aarch64-linux-gnu -static -O2 -o perfbench perfbench.c
 */
#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <time.h>
#include <sys/mman.h>
#include <sys/wait.h>
#include <sys/syscall.h>
#include <fcntl.h>

static double now(void)
{
	struct timespec ts;

	clock_gettime(CLOCK_MONOTONIC, &ts);
	return ts.tv_sec + ts.tv_nsec / 1e9;
}

static void report(const char *name, const char *unit, double secs, long ops)
{
	printf("%-10s %10.3f us/%s   (%ld ops in %.2fs)\n",
	       name, secs * 1e6 / ops, unit, ops, secs);
	fflush(stdout);
}

/* Deliberately not optimisable away: the result is printed. */
static unsigned long compute(long iters)
{
	unsigned long acc = 12345;
	long i;

	for (i = 0; i < iters; i++)
		acc = acc * 6364136223846793005UL + 1442695040888963407UL;
	return acc;
}

int main(int argc, char **argv)
{
	long syscalls = 200000, faults = 20000, execs = 300, cpu = 200000000;
	double t0, t1;
	long i;

	if (argc > 1 && strcmp(argv[1], "--quick") == 0) {
		syscalls = 20000; faults = 4000; execs = 60; cpu = 20000000;
	}

	setvbuf(stdout, NULL, _IONBF, 0);
	printf("PERFBENCH_START\n");

	/* --- compute: should be native --- */
	t0 = now();
	{
		unsigned long r = compute(cpu);

		t1 = now();
		printf("compute    %10.3f ns/iter  (checksum %lu)\n",
		       (t1 - t0) * 1e9 / cpu, r);
	}

	/* --- syscall --- */
	t0 = now();
	for (i = 0; i < syscalls; i++)
		syscall(SYS_getppid);
	t1 = now();
	report("syscall", "call", t1 - t0, syscalls);

	/*
	 * --- openat: a syscall that takes a path ---
	 *
	 * getppid() above is the cheapest possible syscall and measures pure
	 * interception cost, which is the right question for UML -- it is the
	 * kernel, so it must service every syscall the guest makes.
	 *
	 * It is the wrong question for proot, which installs a seccomp filter
	 * and only traps the syscalls it has to rewrite. getppid() is not one of
	 * them and runs at native speed there, so comparing on it alone would
	 * flatter proot by measuring nothing. A path-taking syscall is work for
	 * both: proot must canonicalise and rewrite the path, UML must service
	 * the open.
	 */
	{
		long n = syscalls / 4;

		t0 = now();
		for (i = 0; i < n; i++) {
			int fd = open("/etc/hostname", O_RDONLY);

			if (fd < 0) {
				fd = open("/proc/self/cmdline", O_RDONLY);
				if (fd < 0) {
					printf("openat     no file to open\n");
					n = 0;
					break;
				}
			}
			close(fd);
		}
		t1 = now();
		if (n)
			report("openat", "open", t1 - t0, n);
	}

	/* --- page fault: fresh anonymous memory, one touch per page --- */
	{
		long pagesz = sysconf(_SC_PAGESIZE);
		size_t len = (size_t)faults * pagesz;
		char *p = mmap(NULL, len, PROT_READ | PROT_WRITE,
			       MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);

		if (p == MAP_FAILED) {
			printf("fault      mmap failed\n");
		} else {
			t0 = now();
			for (i = 0; i < faults; i++)
				p[(size_t)i * pagesz] = 1;
			t1 = now();
			report("fault", "page", t1 - t0, faults);
			munmap(p, len);
		}
	}

	/* --- fork + exec + wait --- */
	t0 = now();
	for (i = 0; i < execs; i++) {
		pid_t pid = fork();

		if (pid == 0) {
			execl("/bin/true", "true", (char *)NULL);
			execl("/bin/busybox", "true", (char *)NULL);
			_exit(127);
		}
		if (pid > 0) {
			int st;

			waitpid(pid, &st, 0);
		}
	}
	t1 = now();
	report("forkexec", "exec", t1 - t0, execs);

	printf("PERFBENCH_DONE\n");
	return 0;
}
