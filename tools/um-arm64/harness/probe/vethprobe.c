// SPDX-License-Identifier: GPL-2.0
/*
 * Create a veth pair with rtnetlink and report whether the kernel made one.
 *
 * The obvious test is "ip link add veth0 type veth peer name veth1", but the
 * shipped rootfs has busybox's ip, and busybox does not know the veth link kind
 * at all -- the string does not appear in the binary. It therefore fails
 * identically whether or not CONFIG_VETH is set, which makes it useless as a
 * check for the thing docker actually needs.
 *
 * So ask the kernel directly. This is the same RTM_NEWLINK message iproute2
 * sends, with IFLA_LINKINFO/IFLA_INFO_KIND set to "veth" and the peer's
 * attributes nested inside IFLA_INFO_DATA/VETH_INFO_PEER.
 *
 * Build:
 *   clang --target=aarch64-linux-gnu -static -O1 -o vethprobe vethprobe.c
 */
#include <stdio.h>
#include <string.h>
#include <errno.h>
#include <unistd.h>
#include <sys/socket.h>
#include <linux/netlink.h>
#include <linux/rtnetlink.h>
#include <linux/veth.h>
#include <linux/if.h>

static void *nest_start(struct nlmsghdr *n, int type)
{
	struct rtattr *a = (void *)((char *)n + NLMSG_ALIGN(n->nlmsg_len));

	a->rta_type = type;
	n->nlmsg_len = NLMSG_ALIGN(n->nlmsg_len) + RTA_LENGTH(0);
	return a;
}

static void nest_end(struct nlmsghdr *n, struct rtattr *a)
{
	a->rta_len = (char *)n + n->nlmsg_len - (char *)a;
}

static void addattr(struct nlmsghdr *n, int type, const void *d, int len)
{
	struct rtattr *a = (void *)((char *)n + NLMSG_ALIGN(n->nlmsg_len));

	a->rta_type = type;
	a->rta_len = RTA_LENGTH(len);
	memcpy(RTA_DATA(a), d, len);
	n->nlmsg_len = NLMSG_ALIGN(n->nlmsg_len) + RTA_ALIGN(RTA_LENGTH(len));
}

int main(void)
{
	struct { struct nlmsghdr n; struct ifinfomsg i; char buf[1024]; } req;
	struct rtattr *linkinfo, *infodata, *peer;
	struct sockaddr_nl sa = { .nl_family = AF_NETLINK };
	struct nlmsghdr *resp;
	char rbuf[4096];
	int fd, n;

	fd = socket(AF_NETLINK, SOCK_RAW, NETLINK_ROUTE);
	if (fd < 0) {
		printf("veth: no netlink socket: %s\n", strerror(errno));
		return 2;
	}
	if (bind(fd, (struct sockaddr *)&sa, sizeof(sa)) < 0) {
		printf("veth: bind: %s\n", strerror(errno));
		return 2;
	}

	memset(&req, 0, sizeof(req));
	req.n.nlmsg_len = NLMSG_LENGTH(sizeof(struct ifinfomsg));
	req.n.nlmsg_flags = NLM_F_REQUEST | NLM_F_CREATE | NLM_F_EXCL | NLM_F_ACK;
	req.n.nlmsg_type = RTM_NEWLINK;
	req.i.ifi_family = AF_UNSPEC;

	addattr(&req.n, IFLA_IFNAME, "vprobe0", sizeof("vprobe0"));
	linkinfo = nest_start(&req.n, IFLA_LINKINFO);
	addattr(&req.n, IFLA_INFO_KIND, "veth", 4);
	infodata = nest_start(&req.n, IFLA_INFO_DATA);
	peer = nest_start(&req.n, VETH_INFO_PEER);
	/* The peer nest carries a bare ifinfomsg before its attributes. */
	req.n.nlmsg_len += NLMSG_ALIGN(sizeof(struct ifinfomsg));
	addattr(&req.n, IFLA_IFNAME, "vprobe1", sizeof("vprobe1"));
	nest_end(&req.n, peer);
	nest_end(&req.n, infodata);
	nest_end(&req.n, linkinfo);

	if (send(fd, &req, req.n.nlmsg_len, 0) < 0) {
		printf("veth: send: %s\n", strerror(errno));
		return 2;
	}

	n = recv(fd, rbuf, sizeof(rbuf), 0);
	if (n < 0) {
		printf("veth: recv: %s\n", strerror(errno));
		return 2;
	}

	resp = (struct nlmsghdr *)rbuf;
	if (resp->nlmsg_type == NLMSG_ERROR) {
		struct nlmsgerr *e = NLMSG_DATA(resp);

		if (e->error == 0) {
			printf("veth: created vprobe0/vprobe1\n");
			return 0;
		}
		printf("veth: kernel refused: %s\n", strerror(-e->error));
		return 1;
	}
	printf("veth: unexpected netlink reply type %d\n", resp->nlmsg_type);
	return 1;
}
