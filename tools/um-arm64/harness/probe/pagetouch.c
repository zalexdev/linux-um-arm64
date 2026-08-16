// SPDX-License-Identifier: GPL-2.0
/*
 * Touch a controlled number of fresh anonymous guest pages, one byte each, and
 * say how long it took. Meant to be run as the guest's init (init=/pagetouch)
 * so that the *host* side of the same run can be counted: the fault cost model
 * claims the UML kernel process takes one host minor fault per host page behind
 * every guest page it zeroes -- four of them per 16K guest page on a 4K host --
 * because clear_page() writes through the MAP_SHARED physmem mapping before the
 * stub ever sees the page.
 *
 * That claim has only ever been checked on an x86_64 UML build where guest and
 * host page sizes are equal, which is precisely the configuration in which the
 * amplification cannot show up. Running this at two different footprints and
 * differencing the host's getrusage(RUSAGE_CHILDREN).ru_minflt across the two
 * runs gives host minor faults per guest page directly, with no instrumentation
 * in the kernel and no rebuild.
 *
 * MB comes from the environment because unrecognised key=value boot arguments
 * are handed to init as environment variables, so the footprint is a boot
 * argument and one initramfs serves every point on the curve.
 *
 * Build:
 *   clang --target=aarch64-linux-gnu -static -O2 -o pagetouch pagetouch.c
 */
#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <time.h>
#include <sys/mman.h>
#include <sys/reboot.h>

static double now(void)
{
	struct timespec ts;

	clock_gettime(CLOCK_MONOTONIC, &ts);
	return ts.tv_sec + ts.tv_nsec / 1e9;
}

int main(void)
{
	const char *e = getenv("MB");
	long mb = e ? atol(e) : 64;
	long pgsz = sysconf(_SC_PAGESIZE);
	size_t len = (size_t)mb * 1024 * 1024;
	long pages = len / pgsz;
	char *p;
	long i;
	double t0, t1;

	printf("PAGETOUCH_START mb=%ld pagesize=%ld pages=%ld\n", mb, pgsz, pages);
	fflush(stdout);

	p = mmap(NULL, len, PROT_READ | PROT_WRITE,
		 MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
	if (p == MAP_FAILED) {
		printf("PAGETOUCH_FAIL mmap\n");
		fflush(stdout);
		goto out;
	}

	t0 = now();
	for (i = 0; i < pages; i++)
		p[i * pgsz] = 1;
	t1 = now();

	printf("touch %10.3f us/page  (%ld pages in %.3fs)\n",
	       (t1 - t0) * 1e6 / pages, pages, t1 - t0);

	/*
	 * The vDSO question, measured rather than argued. arm64 UML exports only
	 * __kernel_rt_sigreturn, so a guest clock_gettime is a full seccomp round
	 * trip; on real hardware it is a counter read of ~25-40 ns. Print it next
	 * to a getppid from the same loop so the difference between "the trap" and
	 * "the trap plus UML's own timer_read" is visible.
	 */
	{
		struct timespec ts;
		long n = 200000, k;

		t0 = now();
		for (k = 0; k < n; k++)
			clock_gettime(CLOCK_MONOTONIC, &ts);
		t1 = now();
		printf("clock_gettime %6.3f us/call (%ld ops)\n",
		       (t1 - t0) * 1e6 / n, n);

		t0 = now();
		for (k = 0; k < n; k++)
			clock_gettime(CLOCK_MONOTONIC_COARSE, &ts);
		t1 = now();
		printf("clock_coarse  %6.3f us/call (%ld ops)\n",
		       (t1 - t0) * 1e6 / n, n);

		t0 = now();
		for (k = 0; k < n; k++)
			getppid();
		t1 = now();
		printf("getppid       %6.3f us/call (%ld ops)\n",
		       (t1 - t0) * 1e6 / n, n);
	}

	printf("PAGETOUCH_DONE\n");
	fflush(stdout);

out:
	sync();
	reboot(RB_POWER_OFF);
	for (;;)
		pause();
}
