// SPDX-License-Identifier: GPL-2.0
/*
 * Is a guest fork()+execve() O(parent RSS)?
 *
 * UML's set_pte() marks every PTE _PAGE_NEEDSYNC, and copy_present_ptes() runs
 * set_ptes() on the child's mm, so after a fork the child's ENTIRE inherited
 * resident set is marked and um_tlb_sync() replays it as host mmap()s into a
 * brand-new stub at the child's first userspace entry -- for a child whose only
 * act is to execve and throw all of it away. The parent is wrprotected in the
 * same pass and re-published a second time. Two proposals in flight rest on
 * that being expensive: "don't publish a forked child's address space until it
 * touches it" and "make a guest fork map onto a host fork".
 *
 * perfbench cannot see any of it: it munmaps its 64 MB fault buffer before the
 * forkexec loop, so its parent RSS is tiny and constant. Run this instead with
 * RSS_MB swept and the answer is direct -- if us/exec is flat in parent RSS the
 * eager publication costs nothing worth removing, and if it rises linearly the
 * slope IS the per-resident-page tax, in us per guest page.
 *
 * Runs as the guest's init. RSS_MB, N and EXE come from the environment because
 * unrecognised key=value boot arguments are handed to init.
 *
 * Build:
 *   clang --target=aarch64-linux-gnu -static -O2 -o forkexecrss forkexecrss.c
 */
#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <time.h>
#include <sys/mman.h>
#include <sys/wait.h>
#include <sys/reboot.h>

static double now(void)
{
	struct timespec ts;

	clock_gettime(CLOCK_MONOTONIC, &ts);
	return ts.tv_sec + ts.tv_nsec / 1e9;
}

int main(void)
{
	const char *e;
	long rss_mb = (e = getenv("RSS_MB")) ? atol(e) : 0;
	long n = (e = getenv("N")) ? atol(e) : 200;
	const char *exe = (e = getenv("EXE")) ? e : "/true";
	long pgsz = sysconf(_SC_PAGESIZE);
	char *argvv[] = { (char *)"true", NULL };
	char *p = NULL;
	double t0, t1;
	long i;

	printf("FORKEXECRSS_START rss_mb=%ld n=%ld exe=%s pagesize=%ld\n",
	       rss_mb, n, exe, pgsz);
	fflush(stdout);

	if (rss_mb > 0) {
		size_t len = (size_t)rss_mb * 1024 * 1024;
		long pages = len / pgsz;

		p = mmap(NULL, len, PROT_READ | PROT_WRITE,
			 MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
		if (p == MAP_FAILED) {
			printf("FORKEXECRSS_FAIL mmap\n");
			fflush(stdout);
			goto out;
		}
		/* Make it resident and writable-private, so the fork has to copy
		 * the PTEs and wrprotect them -- the case the proposals target. */
		for (i = 0; i < pages; i++)
			p[i * pgsz] = 1;
	}

	/* One warm-up exec so the binary's pages are in the guest page cache. */
	{
		pid_t pid = fork();

		if (pid == 0) {
			execve(exe, argvv, NULL);
			_exit(127);
		}
		waitpid(pid, NULL, 0);
	}

	t0 = now();
	for (i = 0; i < n; i++) {
		pid_t pid = fork();

		if (pid == 0) {
			execve(exe, argvv, NULL);
			_exit(127);
		}
		if (pid < 0)
			break;
		waitpid(pid, NULL, 0);
	}
	t1 = now();

	printf("forkexec %10.3f us/exec  (rss_mb=%ld, %ld ops in %.3fs)\n",
	       (t1 - t0) * 1e6 / i, rss_mb, i, t1 - t0);

	/* fork+_exit alone, same RSS: separates the exec from the fork. */
	t0 = now();
	for (i = 0; i < n; i++) {
		pid_t pid = fork();

		if (pid == 0)
			_exit(0);
		if (pid < 0)
			break;
		waitpid(pid, NULL, 0);
	}
	t1 = now();
	printf("forkexit %10.3f us/op    (rss_mb=%ld, %ld ops in %.3fs)\n",
	       (t1 - t0) * 1e6 / i, rss_mb, i, t1 - t0);

	printf("FORKEXECRSS_DONE\n");
	fflush(stdout);
out:
	sync();
	reboot(RB_POWER_OFF);
	for (;;)
		pause();
}
