// SPDX-License-Identifier: GPL-2.0
/*
 * What does UML's per-mm host process actually cost on this phone?
 *
 * Every guest mm gets its own host process. A guest fork()+execve() therefore
 * costs the host TWO clone+execveat of the stub binary and TWO SIGKILL +
 * blocking waitpid, where a native kernel would do one fork and one exec. The
 * "recycle stubs instead of creating one per mm" proposal removes exactly that,
 * and its whole case rests on how much those four operations cost -- a number
 * nobody has measured on an arm64 phone. Estimates in the tree span 350-700 us
 * for the creates and 180-380 us for the destroys, which is wide enough that
 * the proposal is either the largest win available or barely worth building.
 *
 * Rows, each the previous plus one thing:
 *
 *   clone_exit   clone(CLONE_VM|CLONE_VFORK|SIGCHLD) + _exit + waitpid
 *                the bare host process lifecycle, no exec
 *   fork_exit    fork() + _exit + waitpid, for reference
 *   clone_tiny   clone + execveat(a 200-byte nostdlib binary) + waitpid
 *                adds one Android execve, SELinux and audit included
 *   clone_stub   clone + execveat(the real uml-userspace stub) + waitpid
 *                the actual binary start_userspace() execs. The stub dies
 *                early reading its init_data from a closed fd 0, so this is
 *                create + exec + its first few syscalls, not a full stub.
 *   kill_wait_N  SIGKILL + blocking waitpid of a live child holding N private
 *                mappings, N in {0, 32, 128, 512}. This is destroy_context()'s
 *                os_kill_ptraced_process(pid, 1), which the UML kernel thread
 *                sits inside with signals blocked, twice per guest fork+exec.
 *
 * Build:
 *   clang --target=aarch64-linux-gnu -static -O2 -o spawnprobe spawnprobe.c
 *
 * Usage: spawnprobe <tiny-exe> <stub-exe> [iters] [rounds]
 */
#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <time.h>
#include <fcntl.h>
#include <sched.h>
#include <signal.h>
#include <errno.h>
#include <sys/mman.h>
#include <sys/wait.h>
#include <sys/syscall.h>

static double now(void)
{
	struct timespec ts;

	clock_gettime(CLOCK_MONOTONIC, &ts);
	return ts.tv_sec + ts.tv_nsec / 1e9;
}

static int cmpd(const void *a, const void *b)
{
	double x = *(const double *)a, y = *(const double *)b;

	return x < y ? -1 : x > y ? 1 : 0;
}

static void report(const char *name, double *v, int n)
{
	qsort(v, n, sizeof(*v), cmpd);
	printf("%-14s %9.2f us/op   min %9.2f  max %9.2f  n=%d\n",
	       name, v[n / 2], v[0], v[n - 1], n);
	fflush(stdout);
}

/*
 * start_userspace() uses CLONE_VFORK|CLONE_VM|SIGCHLD and a stack it allocated
 * itself; reproduce that rather than plain fork(), because CLONE_VFORK suspends
 * the parent for the whole clone->execve window and that suspension is part of
 * what pooling would remove.
 */
static long do_clone(void *stack_top)
{
	return syscall(__NR_clone, CLONE_VFORK | CLONE_VM | SIGCHLD,
		       stack_top, NULL, NULL, NULL);
}

static char *stack_alloc(void)
{
	char *s = mmap(NULL, 256 * 1024, PROT_READ | PROT_WRITE,
		       MAP_PRIVATE | MAP_ANONYMOUS | MAP_STACK, -1, 0);

	return s == MAP_FAILED ? NULL : s + 256 * 1024;
}

static double bench_clone_exit(long iters)
{
	char *st = stack_alloc();
	double t0 = now();
	long i;

	for (i = 0; i < iters; i++) {
		long pid = do_clone(st);

		if (pid == 0)
			syscall(__NR_exit, 0);
		if (pid < 0)
			return -1;
		waitpid(pid, NULL, 0);
	}
	return (now() - t0) * 1e6 / iters;
}

static double bench_fork_exit(long iters)
{
	double t0 = now();
	long i;

	for (i = 0; i < iters; i++) {
		pid_t pid = fork();

		if (pid == 0)
			syscall(__NR_exit, 0);
		if (pid < 0)
			return -1;
		waitpid(pid, NULL, 0);
	}
	return (now() - t0) * 1e6 / iters;
}

/*
 * clone + execveat(fd) + wait. Uses raw execveat on an already-open fd, which
 * is exactly what userspace_tramp() does, so the path-resolution cost is out of
 * the measurement and only the exec itself is in it.
 */
static double bench_clone_exec(long iters, int fd)
{
	char *st = stack_alloc();
	char *argv[] = { (char *)"stub", NULL };
	double t0 = now();
	long i;

	for (i = 0; i < iters; i++) {
		long pid = do_clone(st);

		if (pid == 0) {
			syscall(__NR_execveat, fd, (unsigned long)"",
				(unsigned long)argv, NULL, AT_EMPTY_PATH);
			syscall(__NR_exit, 5);
		}
		if (pid < 0)
			return -1;
		waitpid(pid, NULL, 0);
	}
	return (now() - t0) * 1e6 / iters;
}

/*
 * SIGKILL + blocking waitpid of a child that has just built `maps` private
 * mappings, which is what a stub carrying a guest address space looks like to
 * the host's exit_mmap(). The child signals readiness through a pipe so the
 * mapping work is outside the timed region.
 */
static double bench_kill_wait(long iters, int maps)
{
	double total = 0;
	long i;

	for (i = 0; i < iters; i++) {
		int p[2];
		pid_t pid;
		char c;
		double t0;

		if (pipe(p))
			return -1;
		pid = fork();
		if (pid == 0) {
			int m;

			close(p[0]);
			for (m = 0; m < maps; m++)
				mmap(NULL, 16384, PROT_READ | PROT_WRITE,
				     MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
			c = 'r';
			if (write(p[1], &c, 1) != 1)
				syscall(__NR_exit, 1);
			for (;;)
				pause();
		}
		close(p[1]);
		if (read(p[0], &c, 1) != 1)
			return -1;
		close(p[0]);

		t0 = now();
		kill(pid, SIGKILL);
		waitpid(pid, NULL, 0);
		total += now() - t0;
	}
	return total * 1e6 / iters;
}

int main(int argc, char **argv)
{
	long iters = argc > 3 ? atol(argv[3]) : 300;
	int rounds = argc > 4 ? atoi(argv[4]) : 7;
	int tiny_fd = -1, stub_fd = -1;
	double v[64];
	int r;
	static const int mapcounts[] = { 0, 32, 128, 512 };
	size_t mi;

	if (argc < 3) {
		fprintf(stderr, "usage: %s <tiny-exe> <stub-exe> [iters] [rounds]\n",
			argv[0]);
		return 2;
	}
	if (rounds > 64)
		rounds = 64;

	tiny_fd = open(argv[1], O_RDONLY);
	stub_fd = open(argv[2], O_RDONLY);
	if (tiny_fd < 0 || stub_fd < 0) {
		perror("open");
		return 1;
	}

	printf("SPAWNPROBE_START iters=%ld rounds=%d\n", iters, rounds);

	for (r = 0; r < rounds; r++)
		v[r] = bench_clone_exit(iters);
	report("clone_exit", v, rounds);

	for (r = 0; r < rounds; r++)
		v[r] = bench_fork_exit(iters);
	report("fork_exit", v, rounds);

	for (r = 0; r < rounds; r++)
		v[r] = bench_clone_exec(iters, tiny_fd);
	report("clone_tiny", v, rounds);

	for (r = 0; r < rounds; r++)
		v[r] = bench_clone_exec(iters, stub_fd);
	report("clone_stub", v, rounds);

	for (mi = 0; mi < sizeof(mapcounts) / sizeof(mapcounts[0]); mi++) {
		char name[32];

		for (r = 0; r < rounds; r++)
			v[r] = bench_kill_wait(iters / 3 + 1, mapcounts[mi]);
		snprintf(name, sizeof(name), "kill_wait_%d", mapcounts[mi]);
		report(name, v, rounds);
	}

	printf("SPAWNPROBE_DONE\n");
	return 0;
}
