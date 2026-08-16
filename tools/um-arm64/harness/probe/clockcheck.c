// SPDX-License-Identifier: GPL-2.0
/*
 * clockcheck -- does a second inside the guest last a second outside it?
 *
 * Every performance number in this tree is a guest-measured interval divided
 * by an operation count, so if the guest's CLOCK_MONOTONIC runs at the wrong
 * rate then every one of them is wrong by that factor and no amount of
 * careful benchmarking notices. The symptom that prompted this: the guest
 * reported a pure userspace arithmetic loop running *faster* than the same
 * binary on the host it runs on, which cannot be true.
 *
 * So: spin until the guest's own clock says SECS have passed, and let the
 * caller time the whole run from outside. Guest-measured and host-measured
 * intervals should agree. They must be compared over an interval long enough
 * that boot time and process startup are noise -- hence seconds, not
 * milliseconds.
 *
 * Prints the guest's own view too, so a run that is off can be told apart
 * from a run that simply took longer to boot.
 *
 * Build:
 *   clang --target=aarch64-linux-gnu -static -O2 -o clockcheck clockcheck.c
 */
#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <time.h>

static double now(clockid_t id)
{
	struct timespec ts;

	clock_gettime(id, &ts);
	return ts.tv_sec + ts.tv_nsec / 1e9;
}

int main(int argc, char **argv)
{
	double secs = argc > 1 ? atof(argv[1]) : 5.0;
	double m0, m1, r0, r1;
	unsigned long spins = 0;

	r0 = now(CLOCK_REALTIME);
	m0 = now(CLOCK_MONOTONIC);
	printf("CLOCKCHECK_START want=%.3f mono=%.6f real=%.6f\n",
	       secs, m0, r0);
	fflush(stdout);

	/*
	 * Busy-wait rather than sleep: a sleep would measure the timer
	 * subsystem's wakeup accuracy, and the question here is the rate at
	 * which the clock advances while userspace runs.
	 */
	do {
		spins++;
		m1 = now(CLOCK_MONOTONIC);
	} while (m1 - m0 < secs);
	r1 = now(CLOCK_REALTIME);

	printf("CLOCKCHECK_DONE mono_elapsed=%.6f real_elapsed=%.6f spins=%lu\n",
	       m1 - m0, r1 - r0, spins);
	return 0;
}
