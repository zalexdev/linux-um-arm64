// SPDX-License-Identifier: GPL-2.0
/*
 * What does re-passing the physmem fd on every mapping handoff cost?
 *
 * Every handoff that carries a queued stub syscall costs three extra host
 * syscalls: sendmsg(SCM_RIGHTS) on the kernel side, recvmsg on the stub side,
 * and one close() per received fd. There is normally exactly one physmem fd,
 * and userspace() resets mm_id->syscall_fd_num to 0 after every handoff, so the
 * same fd is installed and torn down again forever. The proposal to make the
 * stub's fd table sticky is worth building or not depending purely on what that
 * costs on this phone, which nobody has measured.
 *
 * Rows:
 *   scm_rights   sendmsg(SCM_RIGHTS, 1 fd) + recvmsg + close, round trip
 *   plain        the same round trip with no ancillary data, so the difference
 *                is the fd install and teardown rather than the socket
 *   recvmsg_eagain  a nonblocking recvmsg on an empty socket -- the stub issues
 *                one of these whenever syscall_data_len != 0 but the kernel
 *                sent no fds, which is the other half of the same proposal
 *
 * Build:
 *   clang --target=aarch64-linux-gnu -static -O2 -o scmprobe scmprobe.c
 */
#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <time.h>
#include <fcntl.h>
#include <errno.h>
#include <sched.h>
#include <sys/socket.h>
#include <sys/wait.h>
#include <sys/mman.h>

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
	printf("%-16s %8.3f us/op   min %8.3f  max %8.3f  n=%d\n",
	       name, v[n / 2], v[0], v[n - 1], n);
	fflush(stdout);
}

static int pin(int cpu)
{
	cpu_set_t s;

	CPU_ZERO(&s);
	CPU_SET(cpu, &s);
	return sched_setaffinity(0, sizeof(s), &s);
}

static int sv[2];
static int payload_fd;

/* One send + one receive + close, all in this process: no peer scheduling in
 * the measurement, which is what we want -- the question is the fd install
 * cost, not the handoff latency (handoff.c measures that). */
static double bench_scm(long iters, int with_fd)
{
	char buf[8] = { 0 };
	union {
		char b[CMSG_SPACE(sizeof(int))];
		struct cmsghdr align;
	} ctrl;
	double t0 = now();
	long i;

	for (i = 0; i < iters; i++) {
		struct msghdr mh;
		struct iovec iov = { buf, sizeof(buf) };
		struct cmsghdr *cm;

		memset(&mh, 0, sizeof(mh));
		mh.msg_iov = &iov;
		mh.msg_iovlen = 1;
		if (with_fd) {
			memset(&ctrl, 0, sizeof(ctrl));
			mh.msg_control = ctrl.b;
			mh.msg_controllen = sizeof(ctrl.b);
			cm = CMSG_FIRSTHDR(&mh);
			cm->cmsg_level = SOL_SOCKET;
			cm->cmsg_type = SCM_RIGHTS;
			cm->cmsg_len = CMSG_LEN(sizeof(int));
			memcpy(CMSG_DATA(cm), &payload_fd, sizeof(int));
		}
		if (sendmsg(sv[0], &mh, 0) < 0)
			return -1;

		memset(&mh, 0, sizeof(mh));
		mh.msg_iov = &iov;
		mh.msg_iovlen = 1;
		if (with_fd) {
			memset(&ctrl, 0, sizeof(ctrl));
			mh.msg_control = ctrl.b;
			mh.msg_controllen = sizeof(ctrl.b);
		}
		if (recvmsg(sv[1], &mh, 0) < 0)
			return -1;
		if (with_fd) {
			cm = CMSG_FIRSTHDR(&mh);
			if (cm && cm->cmsg_type == SCM_RIGHTS) {
				int got;

				memcpy(&got, CMSG_DATA(cm), sizeof(int));
				close(got);
			}
		}
	}
	return (now() - t0) * 1e6 / iters;
}

/* The recvmsg the stub issues when there is nothing to receive. */
static double bench_eagain(long iters)
{
	char buf[8];
	struct iovec iov = { buf, sizeof(buf) };
	double t0 = now();
	long i;

	for (i = 0; i < iters; i++) {
		struct msghdr mh;

		memset(&mh, 0, sizeof(mh));
		mh.msg_iov = &iov;
		mh.msg_iovlen = 1;
		recvmsg(sv[1], &mh, MSG_DONTWAIT);
	}
	return (now() - t0) * 1e6 / iters;
}

int main(int argc, char **argv)
{
	long iters = argc > 1 ? atol(argv[1]) : 200000;
	int rounds = argc > 2 ? atoi(argv[2]) : 7;
	double v[64];
	int r;

	if (rounds > 64)
		rounds = 64;
	if (socketpair(AF_UNIX, SOCK_STREAM, 0, sv)) {
		perror("socketpair");
		return 1;
	}
	payload_fd = open("/dev/zero", O_RDONLY);
	if (payload_fd < 0) {
		perror("open");
		return 1;
	}
	pin(4);

	printf("SCMPROBE_START\n");

	for (r = 0; r < rounds; r++)
		v[r] = bench_scm(iters, 1);
	report("scm_rights", v, rounds);

	for (r = 0; r < rounds; r++)
		v[r] = bench_scm(iters, 0);
	report("plain", v, rounds);

	for (r = 0; r < rounds; r++)
		v[r] = bench_eagain(iters);
	report("recvmsg_eagain", v, rounds);

	printf("SCMPROBE_DONE\n");
	return 0;
}
