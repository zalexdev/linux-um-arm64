// SPDX-License-Identifier: GPL-2.0
/*
 * umnet -- give a UML guest a network without root, a tap device, or any
 * capability at all.
 *
 * The problem: every conventional UML network transport needs privileges the
 * `shell` user on Android does not have. tap and raw need CAP_NET_ADMIN and
 * /dev/net/tun; gre and l2tpv3 need raw sockets. What is left is UML's `fd`
 * transport, which simply reads and writes Ethernet frames on a descriptor
 * somebody else set up -- and a userspace TCP/IP stack on the other end of it,
 * which needs no privileges because it makes ordinary outbound socket calls on
 * the guest's behalf.
 *
 * passt is that stack. It is plain C with no dependencies beyond libc, so
 * unlike libslirp (which needs glib) it cross-compiles statically for Android
 * in one step.
 *
 * The one mismatch is framing, and it is why this program exists:
 *
 *   UML's fd transport does one frame per datagram, using recvmmsg/sendmmsg on
 *   the descriptor. Frame boundaries come from the socket.
 *
 *   passt speaks qemu's socket protocol on a stream: each frame is preceded by
 *   its length as a 4-byte big-endian integer. Frame boundaries come from the
 *   prefix.
 *
 * So umnet sits between them: a SOCK_DGRAM socketpair towards UML, a
 * SOCK_STREAM socketpair towards passt, and a pump that adds the length prefix
 * in one direction and strips it in the other. It then execs the UML kernel
 * with the guest end of the datagram pair inherited, appending the vector
 * argument that points at it.
 *
 * Usage:
 *   umnet [--passt PATH] [--mac MAC] -- ./linux <uml args...>
 *
 * The UML side ends up with, appended to its command line:
 *   vec0:transport=fd,fd=<n>,mac=<mac>
 */
#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>
#include <signal.h>
#include <fcntl.h>
#include <poll.h>
#include <sys/socket.h>
#include <sys/wait.h>
#include <arpa/inet.h>

#define MAX_FRAME	65536
#define PFX		4		/* qemu socket protocol length prefix */

static pid_t passt_pid;
static int verbose;			/* --verbose: log every frame both ways */

static void reap(int sig)
{
	(void)sig;
	if (passt_pid > 0)
		kill(passt_pid, SIGTERM);
	_exit(0);
}

/* Read exactly n bytes, or return 0 on EOF / -1 on error. */
static int read_full(int fd, void *buf, size_t n)
{
	size_t done = 0;

	while (done < n) {
		ssize_t r = read(fd, (char *)buf + done, n - done);

		if (r == 0)
			return 0;
		if (r < 0) {
			if (errno == EINTR)
				continue;
			return -1;
		}
		done += r;
	}
	return 1;
}

static int write_full(int fd, const void *buf, size_t n)
{
	size_t done = 0;

	while (done < n) {
		ssize_t w = write(fd, (const char *)buf + done, n - done);

		if (w < 0) {
			if (errno == EINTR)
				continue;
			return -1;
		}
		done += w;
	}
	return 0;
}

/*
 * Pump frames both ways until either side closes.
 *
 * Deliberately single-threaded with poll() rather than two threads: the two
 * directions share nothing, the traffic a phone guest generates is far below
 * what one core can shuffle, and a second thread would only add a way to get
 * the shutdown ordering wrong.
 */
static void pump(int uml_fd, int passt_fd)
{
	static unsigned char frame[PFX + MAX_FRAME];
	struct pollfd pfd[2];

	pfd[0].fd = uml_fd;
	pfd[1].fd = passt_fd;

	for (;;) {
		pfd[0].events = pfd[1].events = POLLIN;

		if (poll(pfd, 2, -1) < 0) {
			if (errno == EINTR)
				continue;
			return;
		}

		/* guest -> host: datagram in, length-prefixed out */
		if (pfd[0].revents & POLLIN) {
			ssize_t n = recv(uml_fd, frame + PFX, MAX_FRAME, 0);

			if (n <= 0 && !(n < 0 && errno == EINTR))
				return;
			if (n > 0) {
				uint32_t len = htonl((uint32_t)n);

				memcpy(frame, &len, PFX);
				if (verbose)
					fprintf(stderr, "umnet: guest->host %zd\n", n);
				if (write_full(passt_fd, frame, PFX + n))
					return;
			}
		}

		/* host -> guest: length-prefixed in, datagram out */
		if (pfd[1].revents & POLLIN) {
			uint32_t len;
			int r = read_full(passt_fd, &len, PFX);

			if (r <= 0)
				return;
			len = ntohl(len);
			if (len == 0 || len > MAX_FRAME) {
				fprintf(stderr, "umnet: bad frame length %u\n", len);
				return;
			}
			if (read_full(passt_fd, frame, len) <= 0)
				return;
			if (verbose)
				fprintf(stderr, "umnet: host->guest %u\n", len);
			/*
			 * A datagram socket has no partial writes, and a frame
			 * dropped here is exactly what a dropped packet is on
			 * real hardware -- so failure is not fatal, the guest's
			 * TCP will retransmit.
			 */
			if (send(uml_fd, frame, len, MSG_DONTWAIT) < 0 &&
			    errno != EAGAIN && errno != EWOULDBLOCK && errno != EINTR)
				return;
		}

		if ((pfd[0].revents | pfd[1].revents) & (POLLERR | POLLHUP))
			return;
	}
}

int main(int argc, char **argv)
{
	const char *passt_path = "./passt";
	const char *mac = "02:00:00:00:00:01";
	const char *iface = NULL;	/* passt -i: which host interface to mirror */
	const char *dns = "1.1.1.1";	/* passt -D: nameserver to announce */
	const char *addr = "10.0.2.15";	/* passt -a: address given to the guest */
	const char *mask = "24";	/* passt -n */
	const char *gw = "10.0.2.2";	/* passt -g: gateway passt emulates */
	int uml_sp[2], passt_sp[2], i, umlarg;
	int status, rc = 0;
	char vecarg[128];
	char **child;
	pid_t uml_pid;

	for (i = 1; i < argc; i++) {
		if (!strcmp(argv[i], "--verbose"))
			verbose = 1;
		else if (!strcmp(argv[i], "--passt") && i + 1 < argc)
			passt_path = argv[++i];
		else if (!strcmp(argv[i], "--mac") && i + 1 < argc)
			mac = argv[++i];
		else if (!strcmp(argv[i], "--iface") && i + 1 < argc)
			iface = argv[++i];
		else if (!strcmp(argv[i], "--dns") && i + 1 < argc)
			dns = argv[++i];
		else if (!strcmp(argv[i], "--addr") && i + 1 < argc)
			addr = argv[++i];
		else if (!strcmp(argv[i], "--mask") && i + 1 < argc)
			mask = argv[++i];
		else if (!strcmp(argv[i], "--gw") && i + 1 < argc)
			gw = argv[++i];
		else if (!strcmp(argv[i], "--"))
			break;
		else {
			fprintf(stderr, "umnet: unknown option %s\n", argv[i]);
			return 2;
		}
	}
	if (i >= argc || strcmp(argv[i], "--")) {
		fprintf(stderr,
			"usage: umnet [--passt PATH] [--mac MAC] -- ./linux <args...>\n");
		return 2;
	}
	umlarg = i + 1;
	if (umlarg >= argc) {
		fprintf(stderr, "umnet: no UML command given\n");
		return 2;
	}

	if (socketpair(AF_UNIX, SOCK_DGRAM, 0, uml_sp) ||
	    socketpair(AF_UNIX, SOCK_STREAM, 0, passt_sp)) {
		perror("umnet: socketpair");
		return 1;
	}

	/*
	 * Give the guest side a generous socket buffer. The default is small
	 * enough that a burst -- an apt download, say -- drops frames on the
	 * floor between passt and UML, which shows up as a stalled TCP
	 * connection rather than as an error anybody can see.
	 */
	{
		int sz = 1 << 20;

		setsockopt(uml_sp[0], SOL_SOCKET, SO_RCVBUF, &sz, sizeof(sz));
		setsockopt(uml_sp[0], SOL_SOCKET, SO_SNDBUF, &sz, sizeof(sz));
		setsockopt(uml_sp[1], SOL_SOCKET, SO_RCVBUF, &sz, sizeof(sz));
		setsockopt(uml_sp[1], SOL_SOCKET, SO_SNDBUF, &sz, sizeof(sz));
	}

	signal(SIGINT, reap);
	signal(SIGTERM, reap);

	passt_pid = fork();
	if (passt_pid < 0) {
		perror("umnet: fork");
		return 1;
	}
	if (passt_pid == 0) {
		char fdbuf[16];

		close(uml_sp[0]);
		close(uml_sp[1]);
		close(passt_sp[1]);
		snprintf(fdbuf, sizeof(fdbuf), "%d", passt_sp[0]);
		/*
		 * -f  stay in the foreground, so this process is the child we
		 *     can signal and wait on rather than a daemon that outlives
		 *     the guest and holds the socket.
		 * -t/-u none  no inbound port forwarding: the guest gets
		 *     outbound connectivity, and nothing on the phone's network
		 *     can reach into it.
		 */
		{
			/*
			 * -i is not optional on Android in practice: the
			 * routing table is per-uid, so passt sees several
			 * default routes and reports "Multiple default IPv4
			 * routes, picked first" before falling back to a local
			 * mode with no external connectivity.
			 *
			 * -D likewise: passt learns nameservers from
			 * /etc/resolv.conf, which Android does not have (DNS
			 * goes through netd), so without it passt gives up with
			 * "No IPv4 nameserver available for DHCP" and the guest
			 * gets no lease.
			 */
			const char *av[32];
			int n = 0;

			av[n++] = "passt";
			av[n++] = "-f";
			av[n++] = "-F"; av[n++] = fdbuf;
			av[n++] = "-t"; av[n++] = "none";
			av[n++] = "-u"; av[n++] = "none";
			/*
			 * Give passt the whole network configuration rather than
			 * letting it discover one. It normally reads the host's
			 * addresses, routes and /etc/resolv.conf, and on Android
			 * all three are unavailable to an ordinary uid: netlink
			 * link enumeration is denied outright (passt reports
			 * "Invalid interface name wlan0: Permission denied"),
			 * the routing table is per-uid so several default routes
			 * are visible, and there is no resolv.conf at all.
			 *
			 * Configured explicitly, passt needs none of that. It
			 * still reaches the network the only way an unprivileged
			 * process can, by opening ordinary sockets, which is
			 * exactly what makes this work without root.
			 */
			av[n++] = "-4";
			av[n++] = "-a"; av[n++] = addr;
			av[n++] = "-n"; av[n++] = mask;
			av[n++] = "-g"; av[n++] = gw;
			if (dns) { av[n++] = "-D"; av[n++] = dns; }
			if (iface) { av[n++] = "-i"; av[n++] = iface; }
			av[n] = NULL;
			execv(passt_path, (char *const *)av);
		}
		perror("umnet: exec passt");
		_exit(127);
	}

	close(passt_sp[0]);

	uml_pid = fork();
	if (uml_pid < 0) {
		perror("umnet: fork");
		return 1;
	}
	if (uml_pid == 0) {
		close(uml_sp[1]);
		close(passt_sp[1]);

		/* The descriptor has to survive exec for UML to use it. */
		if (fcntl(uml_sp[0], F_SETFD, 0) < 0) {
			perror("umnet: fcntl");
			_exit(1);
		}

		snprintf(vecarg, sizeof(vecarg),
			 "vec0:transport=fd,fd=%d,mac=%s", uml_sp[0], mac);

		child = calloc(argc - umlarg + 2, sizeof(char *));
		if (!child)
			_exit(1);
		for (i = umlarg; i < argc; i++)
			child[i - umlarg] = argv[i];
		child[argc - umlarg] = vecarg;

		execvp(child[0], child);
		perror("umnet: exec uml");
		_exit(127);
	}

	close(uml_sp[0]);

	pump(uml_sp[1], passt_sp[1]);

	/*
	 * The guest is gone or the link broke; take passt with it.
	 *
	 * Reap the kernel with a blocking wait and report how it died. This was
	 * WNOHANG with a discarded status and an unconditional "return 0", which
	 * made every possible failure -- a kernel killed by a signal, a kernel
	 * that panicked, a kernel that never started -- reach the caller as
	 * "exited, status 0". Inside an Android app, where a sandbox can kill the
	 * process outright and there is no shell to ask, that is the difference
	 * between a diagnosis and a guess.
	 */
	kill(passt_pid, SIGTERM);

	if (waitpid(uml_pid, &status, 0) < 0) {
		perror("umnet: waitpid");
		rc = 1;
	} else if (WIFSIGNALED(status)) {
		fprintf(stderr, "umnet: kernel killed by signal %d (%s)%s\n",
			WTERMSIG(status), strsignal(WTERMSIG(status)),
			WCOREDUMP(status) ? ", core dumped" : "");
		rc = 128 + WTERMSIG(status);
	} else if (WIFEXITED(status)) {
		rc = WEXITSTATUS(status);
		if (rc)
			fprintf(stderr, "umnet: kernel exited %d\n", rc);
	}

	if (waitpid(passt_pid, &status, 0) > 0) {
		if (WIFSIGNALED(status))
			fprintf(stderr, "umnet: passt killed by signal %d (%s)\n",
				WTERMSIG(status), strsignal(WTERMSIG(status)));
		else if (WIFEXITED(status) && WEXITSTATUS(status))
			fprintf(stderr, "umnet: passt exited %d\n",
				WEXITSTATUS(status));
	}

	return rc;
}
