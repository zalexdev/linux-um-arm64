// SPDX-License-Identifier: GPL-2.0
/*
 * Run a command and report what the HOST spent on it, not what it says about
 * itself: getrusage(RUSAGE_CHILDREN) after the whole descendant tree has been
 * reaped, so a UML run's minor faults, major faults and context switches -- the
 * UML kernel process and every stub it forked -- come out as one number.
 *
 * The point is the fault cost model. It claims the UML kernel process takes one
 * host minor fault per HOST page behind each guest page it zeroes, four per 16K
 * guest page on a 4K host, because clear_page() is the first write through the
 * MAP_SHARED physmem mapping. Run a guest that touches a controlled number of
 * pages (harness/probe/pagetouch.c) at two footprints under this wrapper, and
 * the slope of ru_minflt against guest pages touched is that claim, measured.
 * A slope near 5 (4 in the kernel process + 1 in the stub) confirms it; a slope
 * near 1 refutes it and takes 60% of the fault row's cost model with it.
 *
 * ru_nvcsw is reported too, because the "the futex handoff is already gone"
 * claim says a spinning handoff performs zero voluntary context switches per
 * guest syscall, and that has never been counted either.
 *
 * Build:
 *   clang --target=aarch64-linux-gnu -static -O2 -o childfaults childfaults.c
 */
#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <sys/wait.h>
#include <sys/resource.h>
#include <sys/time.h>

int main(int argc, char **argv)
{
	struct rusage ru;
	pid_t pid;
	int st;

	if (argc < 2) {
		fprintf(stderr, "usage: %s <cmd> [args...]\n", argv[0]);
		return 2;
	}

	pid = fork();
	if (pid == 0) {
		execvp(argv[1], argv + 1);
		perror("exec");
		_exit(127);
	}
	if (pid < 0) {
		perror("fork");
		return 1;
	}
	waitpid(pid, &st, 0);

	if (getrusage(RUSAGE_CHILDREN, &ru)) {
		perror("getrusage");
		return 1;
	}
	printf("CHILDRUSAGE minflt=%ld majflt=%ld nvcsw=%ld nivcsw=%ld "
	       "utime=%ld.%06ld stime=%ld.%06ld status=%d\n",
	       ru.ru_minflt, ru.ru_majflt, ru.ru_nvcsw, ru.ru_nivcsw,
	       (long)ru.ru_utime.tv_sec, (long)ru.ru_utime.tv_usec,
	       (long)ru.ru_stime.tv_sec, (long)ru.ru_stime.tv_usec, st);
	return 0;
}
