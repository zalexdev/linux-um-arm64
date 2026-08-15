/*
 * notifprobe: settle, on the actual Poco F3 4.19.246 host, which syscall
 * interception mechanisms exist for an unprivileged (uid 2000) process,
 * and measure the raw cross-core handoff primitives that bound what any
 * mechanism can achieve.
 *
 * Part 1: availability
 *   - SECCOMP_GET_ACTION_AVAIL for RET_TRAP / RET_USER_NOTIF
 *   - SECCOMP_SET_MODE_FILTER with FILTER_FLAG_NEW_LISTENER + NULL prog
 *     (EFAULT = flag known, EINVAL = flag unknown)
 *   - userfaultfd(0)                       (unprivileged ok on 4.19?)
 *   - pidfd_open, process_madvise           (expected ENOSYS on 4.19)
 *   - PR_SET_SYSCALL_USER_DISPATCH          (expected EINVAL on 4.19)
 *
 * Part 2: if USER_NOTIF is present, an end-to-end latency benchmark:
 *   child installs filter trapping getppid -> USER_NOTIF, parent RECV/SEND
 *   loop, child calls getppid N times; report ns/call.
 *
 * Part 3: mechanism-floor benchmarks (always run):
 *   - cross-process SHARED (non-private) futex ping-pong, cross-core
 *   - pipe ping-pong (sleep-in-kernel + wake, the user_notif analogue)
 *   Both pinned: side A on core 4, side B on core 5 (SD870 A77s).
 */
#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <unistd.h>
#include <sched.h>
#include <time.h>
#include <stdint.h>
#include <sys/syscall.h>
#include <sys/mman.h>
#include <sys/wait.h>
#include <sys/prctl.h>
#include <sys/ioctl.h>
#include <linux/seccomp.h>
#include <linux/filter.h>
#include <linux/audit.h>
#include <signal.h>
#include <stdarg.h>
#include <stddef.h>
#include <sys/socket.h>

static int socketpair_hack(int sv[2]);
static int send_fd(int sock, int fd);
static int recv_fd(int sock);

#ifndef SECCOMP_GET_ACTION_AVAIL
#define SECCOMP_GET_ACTION_AVAIL 2
#endif
#ifndef SECCOMP_RET_USER_NOTIF
#define SECCOMP_RET_USER_NOTIF 0x7fc00000U
#endif
#ifndef SECCOMP_FILTER_FLAG_NEW_LISTENER
#define SECCOMP_FILTER_FLAG_NEW_LISTENER (1UL << 3)
#endif
#ifndef SECCOMP_FILTER_FLAG_TSYNC
#define SECCOMP_FILTER_FLAG_TSYNC (1UL << 0)
#endif

/* seccomp notif structs/ioctls, defined locally so old headers suffice */
struct my_seccomp_notif {
	uint64_t id;
	uint32_t pid;
	uint32_t flags;
	struct seccomp_data data;
};
struct my_seccomp_notif_resp {
	uint64_t id;
	int64_t val;
	int32_t error;
	uint32_t flags;
};
struct my_seccomp_notif_sizes {
	uint16_t seccomp_notif;
	uint16_t seccomp_notif_resp;
	uint16_t seccomp_data;
};
#ifndef SECCOMP_GET_NOTIF_SIZES
#define SECCOMP_GET_NOTIF_SIZES 3
#endif
#define MY_SECCOMP_IOC_MAGIC '!'
#define MY_SECCOMP_IOCTL_NOTIF_RECV  _IOWR(MY_SECCOMP_IOC_MAGIC, 0, struct my_seccomp_notif)
#define MY_SECCOMP_IOCTL_NOTIF_SEND  _IOWR(MY_SECCOMP_IOC_MAGIC, 1, struct my_seccomp_notif_resp)

static void kv(const char *k, const char *fmt, ...)
{
	va_list ap;
	printf("%-28s ", k);
	va_start(ap, fmt);
	vprintf(fmt, ap);
	va_end(ap);
	printf("\n");
	fflush(stdout);
}

static uint64_t now_ns(void)
{
	struct timespec ts;
	clock_gettime(CLOCK_MONOTONIC, &ts);
	return (uint64_t)ts.tv_sec * 1000000000ull + ts.tv_nsec;
}

static void pin(int cpu)
{
	cpu_set_t s;
	CPU_ZERO(&s);
	CPU_SET(cpu, &s);
	if (sched_setaffinity(0, sizeof(s), &s) != 0)
		kv("pin.warn", "cpu%d: %s", cpu, strerror(errno));
}

/* ---------------- Part 1: availability ---------------- */
static int have_user_notif, have_new_listener;

static void probe_avail(void)
{
	unsigned int act;
	int rc;

	act = SECCOMP_RET_TRAP;
	rc = syscall(SYS_seccomp, SECCOMP_GET_ACTION_AVAIL, 0, &act);
	kv("seccomp.ret_trap", "%s", rc == 0 ? "yes" : strerror(errno));

	act = SECCOMP_RET_USER_NOTIF;
	rc = syscall(SYS_seccomp, SECCOMP_GET_ACTION_AVAIL, 0, &act);
	have_user_notif = (rc == 0);
	kv("seccomp.user_notif", "%s", rc == 0 ? "yes" : strerror(errno));

	rc = syscall(SYS_seccomp, SECCOMP_SET_MODE_FILTER,
		     SECCOMP_FILTER_FLAG_NEW_LISTENER, NULL);
	have_new_listener = (rc < 0 && errno == EFAULT);
	kv("seccomp.flag_new_listener", "%s",
	   (rc < 0 && errno == EFAULT) ? "yes" : strerror(errno));

	struct my_seccomp_notif_sizes sz;
	rc = syscall(SYS_seccomp, SECCOMP_GET_NOTIF_SIZES, 0, &sz);
	if (rc == 0)
		kv("seccomp.notif_sizes", "notif=%u resp=%u data=%u",
		   sz.seccomp_notif, sz.seccomp_notif_resp, sz.seccomp_data);
	else
		kv("seccomp.notif_sizes", "%s", strerror(errno));

	rc = syscall(__NR_userfaultfd, 0);
	if (rc >= 0) {
		kv("userfaultfd", "yes (fd=%d)", rc);
		close(rc);
	} else
		kv("userfaultfd", "%s", strerror(errno));

#ifndef __NR_pidfd_open
#define __NR_pidfd_open 434
#endif
	rc = syscall(__NR_pidfd_open, getpid(), 0);
	if (rc >= 0) { kv("pidfd_open", "yes"); close(rc); }
	else kv("pidfd_open", "%s", strerror(errno));

#ifndef PR_SET_SYSCALL_USER_DISPATCH
#define PR_SET_SYSCALL_USER_DISPATCH 59
#endif
	rc = prctl(PR_SET_SYSCALL_USER_DISPATCH, 0, 0, 0, 0);
	kv("syscall_user_dispatch", "%s", rc == 0 ? "yes" : strerror(errno));
}

/* ---------------- Part 2: user_notif RTT if available ---------------- */
#define NOTIF_ITERS 20000

static void bench_user_notif(void)
{
	if (!have_user_notif || !have_new_listener) {
		kv("bench.user_notif", "skipped (unavailable)");
		return;
	}

	struct sock_filter filt[] = {
		BPF_STMT(BPF_LD | BPF_W | BPF_ABS,
			 offsetof(struct seccomp_data, nr)),
		BPF_JUMP(BPF_JMP | BPF_JEQ | BPF_K, __NR_getppid, 0, 1),
		BPF_STMT(BPF_RET | BPF_K, SECCOMP_RET_USER_NOTIF),
		BPF_STMT(BPF_RET | BPF_K, SECCOMP_RET_ALLOW),
	};
	struct sock_fprog prog = {
		.len = sizeof(filt) / sizeof(filt[0]),
		.filter = filt,
	};
	int sv[2];
	if (socketpair_hack(sv))
		return;
	pid_t pid = fork();
	if (pid == 0) {
		pin(4);
		close(sv[0]);
		prctl(PR_SET_NO_NEW_PRIVS, 1, 0, 0, 0);
		int lfd = syscall(SYS_seccomp, SECCOMP_SET_MODE_FILTER,
				  SECCOMP_FILTER_FLAG_NEW_LISTENER, &prog);
		if (lfd < 0)
			_exit(10);
		/* pass fd to parent */
		if (send_fd(sv[1], lfd))
			_exit(11);
		close(lfd);
		/* warmup + timed loop */
		for (int i = 0; i < 1000; i++)
			syscall(__NR_getppid);
		uint64_t t0 = now_ns();
		for (int i = 0; i < NOTIF_ITERS; i++)
			syscall(__NR_getppid);
		uint64_t t1 = now_ns();
		uint64_t per = (t1 - t0) / NOTIF_ITERS;
		/* report via exit pipe */
		write(sv[1], &per, sizeof(per));
		_exit(0);
	}
	pin(5);
	close(sv[1]);
	int lfd = recv_fd(sv[0]);
	if (lfd < 0) {
		kv("bench.user_notif", "fd pass failed");
		kill(pid, SIGKILL);
		waitpid(pid, NULL, 0);
		return;
	}
	struct my_seccomp_notif req;
	struct my_seccomp_notif_resp resp;
	for (;;) {
		memset(&req, 0, sizeof(req));
		if (ioctl(lfd, MY_SECCOMP_IOCTL_NOTIF_RECV, &req) != 0) {
			if (errno == EINTR)
				continue;
			break; /* child exited */
		}
		memset(&resp, 0, sizeof(resp));
		resp.id = req.id;
		resp.val = 42;
		ioctl(lfd, MY_SECCOMP_IOCTL_NOTIF_SEND, &resp);
	}
	uint64_t per = 0;
	/* child already wrote result before exiting; try read */
	read(sv[0], &per, sizeof(per));
	waitpid(pid, NULL, 0);
	kv("bench.user_notif_ns", "%llu", (unsigned long long)per);
}

/* fd passing helpers */
static int socketpair_hack(int sv[2])
{
	if (socketpair(AF_UNIX, SOCK_STREAM, 0, sv) != 0) {
		kv("socketpair", "%s", strerror(errno));
		return -1;
	}
	return 0;
}
static int send_fd(int sock, int fd)
{
	struct msghdr msg = {0};
	struct iovec iov;
	char c = 'x';
	char buf[CMSG_SPACE(sizeof(int))];
	iov.iov_base = &c;
	iov.iov_len = 1;
	msg.msg_iov = &iov;
	msg.msg_iovlen = 1;
	msg.msg_control = buf;
	msg.msg_controllen = sizeof(buf);
	struct cmsghdr *cm = CMSG_FIRSTHDR(&msg);
	cm->cmsg_level = SOL_SOCKET;
	cm->cmsg_type = SCM_RIGHTS;
	cm->cmsg_len = CMSG_LEN(sizeof(int));
	memcpy(CMSG_DATA(cm), &fd, sizeof(int));
	return sendmsg(sock, &msg, 0) == 1 ? 0 : -1;
}
static int recv_fd(int sock)
{
	struct msghdr msg = {0};
	struct iovec iov;
	char c;
	char buf[CMSG_SPACE(sizeof(int))];
	iov.iov_base = &c;
	iov.iov_len = 1;
	msg.msg_iov = &iov;
	msg.msg_iovlen = 1;
	msg.msg_control = buf;
	msg.msg_controllen = sizeof(buf);
	if (recvmsg(sock, &msg, 0) != 1)
		return -1;
	struct cmsghdr *cm = CMSG_FIRSTHDR(&msg);
	if (!cm || cm->cmsg_type != SCM_RIGHTS)
		return -1;
	int fd;
	memcpy(&fd, CMSG_DATA(cm), sizeof(int));
	return fd;
}

/* ---------------- Part 3: handoff floor benchmarks ---------------- */
#define PP_ITERS 20000

/* Cross-process, MAP_SHARED (non-private futex key) ping-pong.
 * A: waits for word==1, sets word=0, wakes.  B: mirror image.
 * One "round trip" = A->B->A, i.e. 2 handoffs = what one guest syscall costs
 * in switches. Reported per ROUND TRIP.
 */
static void bench_futex_pp(int cpu_a, int cpu_b, const char *tag)
{
	volatile uint32_t *w = mmap(NULL, 4096, PROT_READ | PROT_WRITE,
				    MAP_SHARED | MAP_ANONYMOUS, -1, 0);
	*w = 0;
	pid_t pid = fork();
	if (pid == 0) {
		pin(cpu_b);
		for (int i = 0; i < PP_ITERS + 1000; i++) {
			while (*w != 1) {
				if (syscall(SYS_futex, w, 0 /*FUTEX_WAIT*/, 0,
					    NULL, NULL, 0) < 0 &&
				    errno != EAGAIN && errno != EINTR)
					_exit(1);
			}
			*w = 0;
			syscall(SYS_futex, w, 1 /*FUTEX_WAKE*/, 1, NULL, NULL, 0);
		}
		_exit(0);
	}
	pin(cpu_a);
	/* warmup */
	for (int i = 0; i < 1000; i++) {
		*w = 1;
		syscall(SYS_futex, w, 1, 1, NULL, NULL, 0);
		while (*w != 0)
			syscall(SYS_futex, w, 0, 1, NULL, NULL, 0);
	}
	uint64_t t0 = now_ns();
	for (int i = 0; i < PP_ITERS; i++) {
		*w = 1;
		syscall(SYS_futex, w, 1, 1, NULL, NULL, 0);
		while (*w != 0)
			syscall(SYS_futex, w, 0, 1, NULL, NULL, 0);
	}
	uint64_t t1 = now_ns();
	waitpid(pid, NULL, 0);
	kv(tag, "%llu ns/rtt", (unsigned long long)((t1 - t0) / PP_ITERS));
	munmap((void *)w, 4096);
}

/* Pipe ping-pong: closest userspace analogue of a user_notif RECV/SEND
 * handoff (sleep in kernel, wake by peer's write). Per round trip. */
static void bench_pipe_pp(int cpu_a, int cpu_b, const char *tag)
{
	int ab[2], ba[2];
	if (pipe(ab) || pipe(ba))
		return;
	pid_t pid = fork();
	if (pid == 0) {
		pin(cpu_b);
		char c;
		for (int i = 0; i < PP_ITERS + 1000; i++) {
			if (read(ab[0], &c, 1) != 1)
				_exit(1);
			if (write(ba[1], &c, 1) != 1)
				_exit(1);
		}
		_exit(0);
	}
	pin(cpu_a);
	char c = 'p';
	for (int i = 0; i < 1000; i++) {
		write(ab[1], &c, 1);
		read(ba[0], &c, 1);
	}
	uint64_t t0 = now_ns();
	for (int i = 0; i < PP_ITERS; i++) {
		write(ab[1], &c, 1);
		read(ba[0], &c, 1);
	}
	uint64_t t1 = now_ns();
	waitpid(pid, NULL, 0);
	kv(tag, "%llu ns/rtt", (unsigned long long)((t1 - t0) / PP_ITERS));
	close(ab[0]); close(ab[1]); close(ba[0]); close(ba[1]);
}

/* Raw syscall cost on this host for scale */
static void bench_raw_syscall(void)
{
	pin(4);
	for (int i = 0; i < 1000; i++)
		syscall(__NR_getppid);
	uint64_t t0 = now_ns();
	for (int i = 0; i < PP_ITERS; i++)
		syscall(__NR_getppid);
	uint64_t t1 = now_ns();
	kv("host.getppid_ns", "%llu", (unsigned long long)((t1 - t0) / PP_ITERS));
}

int main(void)
{
	struct utsname_hack { char sysname[65], nodename[65], release[65],
		version[65], machine[65], domain[65]; } u;
	if (syscall(SYS_uname, &u) == 0)
		kv("host.release", "%s %s", u.release, u.machine);
	kv("host.uid", "%d", getuid());

	probe_avail();
	bench_raw_syscall();
	bench_futex_pp(4, 5, "futex_pp.cross(4,5)");
	bench_futex_pp(4, 4, "futex_pp.same(4,4)");
	bench_pipe_pp(4, 5, "pipe_pp.cross(4,5)");
	bench_pipe_pp(4, 4, "pipe_pp.same(4,4)");
	bench_user_notif();
	kv("probe.complete", "yes");
	return 0;
}
