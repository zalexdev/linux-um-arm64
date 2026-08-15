// SPDX-License-Identifier: GPL-2.0
/*
 * umusb -- export one already-open usbfs device to a UML guest over USB/IP.
 *
 * An Android app may not open /dev/bus/usb itself: the nodes are root:usb and
 * an app uid is not in that group. What an app may do is ask the user for
 * permission for one device and receive an *already open* descriptor from
 * UsbDeviceConnection.getFileDescriptor(). Every USBDEVFS_* ioctl works on
 * that descriptor -- this is exactly how libusb runs on Android, where
 * libusb_wrap_sys_device() takes the same number -- so an ordinary C helper
 * that inherits the fd can drive the device with no privilege at all.
 *
 * The guest speaks USB/IP because it is the only USB transport a kernel with
 * no host controller can use: vhci_hcd is a virtual root hub whose wire is a
 * socket. umusb is the other end of that socket, i.e. the USB/IP *server* --
 * what the kernel tree calls usbip-host / stub -- turning each CMD_SUBMIT into
 * a USBDEVFS_SUBMITURB and each reaped completion back into a RET_SUBMIT.
 *
 * Usage:
 *   umusb --usbfd N [--listen [ADDR:]PORT] [--verbose] -- ./linux <uml args>
 *
 * Two ways to hand the stream to the guest, because the guest sits in a
 * different fd namespace than we do:
 *
 *   default   A socketpair; the kernel process inherits one end and umusb
 *             serves the other. The inherited number is printed and appended
 *             to the kernel command line as umusb.fd=<n> (see below).
 *
 *   --listen  A TCP listener on the loopback. The guest reaches it through
 *             the existing umnet/passt path: passt maps the gateway address
 *             it hands the guest (10.0.2.2 by default) onto the host's
 *             loopback, so a guest connect() to 10.0.2.2:<port> arrives here
 *             as an ordinary accept(). This is the mode that needs nothing
 *             new in the guest kernel: what vhci_hcd's attach wants is a
 *             *guest* socket fd -- attach_store() calls sockfd_lookup() in the
 *             fd table of the process writing to sysfs -- and a host fd
 *             inherited across exec is not that. A UML guest cannot look up a
 *             host descriptor; something in the guest has to hold the socket.
 *
 * Either way the numbers the guest needs are printed at start-up and, unless
 * --no-cmdline is given, appended to the UML command line:
 *
 *   umusb.fd=<n> umusb.devid=<d> umusb.speed=<s> umusb.busid=<b>
 *
 * with umusb.port=<p> in place of umusb.fd in --listen mode.
 *
 * The kernel does not know those parameters; UML's linux_main() passes every
 * argument it does not recognise straight through to the kernel command line,
 * and init/main.c drops unknown *dotted* parameters silently (they look like
 * parameters of an unloaded module), so they cost one line in /proc/cmdline
 * and no boot noise. The guest side reads them from there and does:
 *
 *   echo "<rhport> <sockfd> <devid> <speed>" > \
 *		/sys/devices/platform/vhci_hcd.0/attach
 *
 * where <rhport> is a free vhci root-hub port (0 will do; the speed decides
 * whether it lands on the high-speed or the super-speed hub, not the number),
 * <sockfd> is the guest's *own* connected socket, and <devid> and <speed> are
 * as printed here: devid is (busnum << 16) | devnum, speed is the kernel's
 * enum usb_device_speed (3 = high). The socket must be SOCK_STREAM -- the
 * attach path rejects anything else.
 *
 * Style note on what is *not* here: no threads. usbfs completions are reaped
 * from the same poll() loop that reads the socket, because the two share the
 * seqnum table and a second thread would only add a way to lose a URB during
 * shutdown.
 */
#define _GNU_SOURCE
#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <poll.h>
#include <signal.h>
#include <stdarg.h>
#include <stddef.h>
#include <sys/un.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/ioctl.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <linux/usbdevice_fs.h>

/* ------------------------------------------------------------------ */
/* USB/IP wire format.
 *
 * Documentation/usb/usbip_protocol.rst and drivers/usb/usbip/usbip_common.h.
 * Every 16- and 32-bit field below is big endian on the wire; the two byte
 * arrays (setup, padding) are not integers and are never swapped. The setup
 * packet in particular stays exactly as the client sent it: it is the USB
 * control request, which is little endian by USB's own definition, and byte
 * swapping it is the classic way to make control transfers fail with
 * -EPIPE on every device.
 */

#define USBIP_VERSION		0x0111	/* v1.1.1, as in configure.ac */

#define OP_REQ_DEVLIST		0x8005
#define OP_REP_DEVLIST		0x0005
#define OP_REQ_IMPORT		0x8003
#define OP_REP_IMPORT		0x0003

#define ST_OK			0x00
#define ST_NA			0x01

#define USBIP_CMD_SUBMIT	1
#define USBIP_CMD_UNLINK	2
#define USBIP_RET_SUBMIT	3
#define USBIP_RET_UNLINK	4

#define USBIP_DIR_OUT		0
#define USBIP_DIR_IN		1

/* uapi/linux/usbip.h: wire flags, deliberately distinct from URB_*. */
#define USBIP_URB_SHORT_NOT_OK	0x0001
#define USBIP_URB_ISO_ASAP	0x0002
#define USBIP_URB_ZERO_PACKET	0x0040
#define USBIP_URB_NO_INTERRUPT	0x0080

#define SYSFS_PATH_MAX		256
#define SYSFS_BUS_ID_SIZE	32

struct op_common {
	uint16_t version;
	uint16_t code;
	uint32_t status;
} __attribute__((packed));

/*
 * The device block is 312 bytes and appears verbatim in both replies: at
 * offset 8 of OP_REP_IMPORT and at offset 12 (after ndev) of OP_REP_DEVLIST,
 * which is where the offsets in the protocol document come from -- busnum at
 * 0x128 in the import reply, 0x12C in the devlist reply.
 */
struct usbip_usb_device {
	char path[SYSFS_PATH_MAX];
	char busid[SYSFS_BUS_ID_SIZE];
	uint32_t busnum;
	uint32_t devnum;
	uint32_t speed;
	uint16_t idVendor;
	uint16_t idProduct;
	uint16_t bcdDevice;
	uint8_t bDeviceClass;
	uint8_t bDeviceSubClass;
	uint8_t bDeviceProtocol;
	uint8_t bConfigurationValue;
	uint8_t bNumConfigurations;
	uint8_t bNumInterfaces;
} __attribute__((packed));

struct usbip_usb_interface {
	uint8_t bInterfaceClass;
	uint8_t bInterfaceSubClass;
	uint8_t bInterfaceProtocol;
	uint8_t padding;
} __attribute__((packed));

/*
 * All four URB PDUs share one 48-byte header: a 20-byte basic header and a
 * 28-byte union. The union arms are padded out here so the union really is 28
 * bytes -- in the kernel the same effect comes from the header being __packed
 * inside a fixed-size read; getting it wrong shifts every payload by 8 bytes
 * and looks exactly like a device that answers garbage.
 */
struct usbip_header {
	uint32_t command;
	uint32_t seqnum;
	uint32_t devid;
	uint32_t direction;
	uint32_t ep;
	union {
		struct {
			uint32_t transfer_flags;
			int32_t transfer_buffer_length;
			int32_t start_frame;
			int32_t number_of_packets;
			int32_t interval;
			unsigned char setup[8];
		} cmd_submit;
		struct {
			int32_t status;
			int32_t actual_length;
			int32_t start_frame;
			int32_t number_of_packets;
			int32_t error_count;
			unsigned char padding[8];
		} ret_submit;
		struct {
			uint32_t seqnum;
			unsigned char padding[24];
		} cmd_unlink;
		struct {
			int32_t status;
			unsigned char padding[24];
		} ret_unlink;
	} u;
} __attribute__((packed));

struct usbip_iso_packet {
	uint32_t offset;
	uint32_t length;		/* expected */
	uint32_t actual_length;
	uint32_t status;
} __attribute__((packed));

_Static_assert(sizeof(struct op_common) == 8, "op_common layout");
_Static_assert(sizeof(struct usbip_usb_device) == 312, "udev layout");
_Static_assert(sizeof(struct usbip_header) == 48, "usbip_header layout");
_Static_assert(sizeof(struct usbip_iso_packet) == 16, "iso descriptor");
_Static_assert(offsetof(struct usbip_header, u.cmd_submit.setup) == 0x28,
	       "setup packet at 0x28");

/* ------------------------------------------------------------------ */
/* USB descriptors -- the few constants used, rather than <linux/usb/ch9.h>,
 * which is one more header to differ between bionic and glibc.
 */
#define USB_DT_DEVICE		0x01
#define USB_DT_CONFIG		0x02
#define USB_DT_INTERFACE	0x04
#define USB_DT_ENDPOINT		0x05

#define USB_DIR_IN		0x80

#define USB_REQ_CLEAR_FEATURE	0x01
#define USB_REQ_SET_FEATURE	0x03
#define USB_REQ_GET_CONFIG	0x08
#define USB_REQ_SET_CONFIG	0x09
#define USB_REQ_SET_INTERFACE	0x0b

#define USB_XFER_CONTROL	0
#define USB_XFER_ISOC		1
#define USB_XFER_BULK		2
#define USB_XFER_INT		3

#define MAX_IFACE		32
#define MAX_DESC		4096	/* enough for any real config set */

/*
 * Anything bigger than this is refused rather than allocated: the field is
 * signed on the wire and a bad length is the first thing a desynchronised
 * stream produces. rtw88's largest bulk transfer is a few tens of kilobytes.
 */
#define MAX_XFER		(1 << 20)
#define MAX_ISO_PACKETS		128	/* usbfs's own limit, devio.c */

struct udev {
	uint16_t vid, pid, bcd;
	uint8_t dclass, dsub, dproto;
	uint8_t nconfigs;
	uint8_t confval;		/* bConfigurationValue now in use */
	uint8_t nifs;
	uint8_t ifnum[MAX_IFACE];
	uint8_t ifclass[MAX_IFACE];
	uint8_t ifsub[MAX_IFACE];
	uint8_t ifproto[MAX_IFACE];
	/* [direction][endpoint number] -> USBDEVFS_URB_TYPE_*, -1 if absent */
	signed char eptype[2][16];
	uint32_t busnum, devnum, speed;
	char busid[SYSFS_BUS_ID_SIZE];
	char path[SYSFS_PATH_MAX];
};

/* ------------------------------------------------------------------ */

static int verbose;
static int usbfd = -1;
static const char *usbsock;	/* --usbsock: receive usbfd over SCM_RIGHTS */
static struct udev dev;
static pid_t uml_pid;

static int mute;	/* selftest: the negative cases are meant to complain */

static void msg(const char *fmt, ...)
{
	va_list ap;

	if (mute)
		return;
	fputs("umusb: ", stderr);
	va_start(ap, fmt);
	vfprintf(stderr, fmt, ap);
	va_end(ap);
	fputc('\n', stderr);
}

static void dbg(const char *fmt, ...)
{
	va_list ap;

	if (!verbose)
		return;
	fputs("umusb: ", stderr);
	va_start(ap, fmt);
	vfprintf(stderr, fmt, ap);
	va_end(ap);
	fputc('\n', stderr);
}

static void onsig(int sig)
{
	(void)sig;
	if (uml_pid > 0)
		kill(uml_pid, SIGTERM);
	_exit(0);
}

/* Read exactly n bytes: 1 on success, 0 on EOF, -1 on error. */
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

/* Throw away n bytes of a PDU we cannot honour: the stream stays framed. */
static int skip_full(int fd, size_t n)
{
	char buf[4096];

	while (n) {
		size_t chunk = n > sizeof(buf) ? sizeof(buf) : n;
		int r = read_full(fd, buf, chunk);

		if (r <= 0)
			return r;
		n -= chunk;
	}
	return 1;
}

static uint16_t get16(const unsigned char *p)
{
	return (uint16_t)(p[0] | (p[1] << 8));	/* USB: little endian */
}

/* ------------------------------------------------------------------ */
/* Endianness. These mirror usbip_header_correct_endian() field for field;
 * the command has to be read before the basic header is swapped when sending
 * and after it is swapped when receiving, which is why it is passed in.
 */

static uint32_t swap32(uint32_t v, int send)
{
	/* htonl and ntohl are the same permutation; the argument only
	 * documents which direction the caller is going.
	 */
	return send ? htonl(v) : ntohl(v);
}

static void hdr_swap_body(struct usbip_header *h, uint32_t cmd, int send)
{
#define SW(f)	((f) = (typeof(f))swap32((uint32_t)(f), send))
	switch (cmd) {
	case USBIP_CMD_SUBMIT:
		SW(h->u.cmd_submit.transfer_flags);
		SW(h->u.cmd_submit.transfer_buffer_length);
		SW(h->u.cmd_submit.start_frame);
		SW(h->u.cmd_submit.number_of_packets);
		SW(h->u.cmd_submit.interval);
		break;
	case USBIP_RET_SUBMIT:
		SW(h->u.ret_submit.status);
		SW(h->u.ret_submit.actual_length);
		SW(h->u.ret_submit.start_frame);
		SW(h->u.ret_submit.number_of_packets);
		SW(h->u.ret_submit.error_count);
		break;
	case USBIP_CMD_UNLINK:
		SW(h->u.cmd_unlink.seqnum);
		break;
	case USBIP_RET_UNLINK:
		SW(h->u.ret_unlink.status);
		break;
	default:
		break;
	}
#undef SW
}

static void hdr_hton(struct usbip_header *h)
{
	uint32_t cmd = h->command;

	h->command = htonl(h->command);
	h->seqnum = htonl(h->seqnum);
	h->devid = htonl(h->devid);
	h->direction = htonl(h->direction);
	h->ep = htonl(h->ep);
	hdr_swap_body(h, cmd, 1);
}

static void hdr_ntoh(struct usbip_header *h)
{
	h->command = ntohl(h->command);
	h->seqnum = ntohl(h->seqnum);
	h->devid = ntohl(h->devid);
	h->direction = ntohl(h->direction);
	h->ep = ntohl(h->ep);
	hdr_swap_body(h, h->command, 0);
}

/* ------------------------------------------------------------------ */
/* Descriptors.
 *
 * usbfs read() returns the 18-byte device descriptor followed by every
 * configuration descriptor. The two halves are not in the same byte order:
 * devio.c's usbdev_read() runs le16_to_cpus() over bcdUSB/idVendor/idProduct/
 * bcdDevice before copying the device descriptor out, while the configuration
 * descriptors are handed over as they came off the wire. On this project's
 * only target -- aarch64, little endian -- the two agree, so everything below
 * parses little endian; on a big-endian host the device descriptor would need
 * native loads instead.
 */
static int parse_descriptors(const unsigned char *b, size_t len,
			     int want_conf, struct udev *d)
{
	size_t pos, end;
	int i;

	memset(d->eptype, -1, sizeof(d->eptype));
	d->nifs = 0;
	d->confval = 0;

	if (len < 18 || b[1] != USB_DT_DEVICE) {
		msg("descriptors: not a device descriptor (%zu bytes)", len);
		return -1;
	}
	d->dclass = b[4];
	d->dsub = b[5];
	d->dproto = b[6];
	d->vid = get16(b + 8);
	d->pid = get16(b + 10);
	d->bcd = get16(b + 12);
	d->nconfigs = b[17];

	pos = 18;
	for (i = 0; i < d->nconfigs; i++) {
		unsigned int total;

		if (pos + 9 > len || b[pos + 1] != USB_DT_CONFIG) {
			msg("descriptors: config %d truncated at %zu", i, pos);
			return -1;
		}
		total = get16(b + pos + 2);
		if (total < 9 || pos + total > len) {
			msg("descriptors: config %d claims %u bytes, have %zu",
			    i, total, len - pos);
			return -1;
		}
		if (want_conf ? b[pos + 5] != want_conf : i != 0) {
			pos += total;
			continue;
		}

		d->confval = b[pos + 5];
		end = pos + total;
		pos += b[pos];
		while (pos + 2 <= end && b[pos] >= 2) {
			const unsigned char *p = b + pos;

			/*
			 * bLength is the device's claim about its own
			 * descriptor, and the fields read below sit inside it.
			 * A device that overstates it -- by accident or on
			 * purpose -- would otherwise walk this loop off the end
			 * of the buffer, which is a stack array here filled
			 * from the wire.
			 */
			if (pos + p[0] > end)
				break;

			if (p[1] == USB_DT_INTERFACE && p[0] >= 9) {
				/*
				 * Only altsetting 0 is reported to the client:
				 * that is what usbip's own read_usb_interface()
				 * does, and the client has the full descriptor
				 * set anyway once it enumerates.
				 */
				if (p[3] == 0 && d->nifs < MAX_IFACE) {
					int n = d->nifs++;

					d->ifnum[n] = p[2];
					d->ifclass[n] = p[5];
					d->ifsub[n] = p[6];
					d->ifproto[n] = p[7];
				}
			} else if (p[1] == USB_DT_ENDPOINT && p[0] >= 7) {
				int epnum = p[2] & 0x0f;
				int dir = (p[2] & USB_DIR_IN) ?
					USBIP_DIR_IN : USBIP_DIR_OUT;
				int type;

				/*
				 * Endpoints of *every* altsetting go into the
				 * table. The guest may SET_INTERFACE at any
				 * time and we would otherwise reject the URBs
				 * that follow; an address that means different
				 * things in two altsettings is not something
				 * real devices do.
				 */
				switch (p[3] & 0x03) {
				case USB_XFER_CONTROL:
					type = USBDEVFS_URB_TYPE_CONTROL;
					break;
				case USB_XFER_ISOC:
					type = USBDEVFS_URB_TYPE_ISO;
					break;
				case USB_XFER_BULK:
					type = USBDEVFS_URB_TYPE_BULK;
					break;
				default:
					type = USBDEVFS_URB_TYPE_INTERRUPT;
					break;
				}
				d->eptype[dir][epnum] = type;
			}
			pos += b[pos];
		}
		pos = end;
	}

	if (!d->confval) {
		msg("descriptors: configuration %d not found", want_conf);
		return -1;
	}
	return 0;
}

/*
 * Which configuration is active? usbfs will not say, so ask the device the
 * way libusb does -- GET_CONFIGURATION is one byte and every device answers
 * it. A device that does not is treated as being in its first configuration,
 * which is true of everything that enumerated successfully.
 */
static int get_configuration(int fd)
{
	struct usbdevfs_ctrltransfer ct;
	unsigned char val = 0;
	int rc;

	memset(&ct, 0, sizeof(ct));
	ct.bRequestType = USB_DIR_IN;		/* device, standard, IN */
	ct.bRequest = USB_REQ_GET_CONFIG;
	ct.wValue = 0;
	ct.wIndex = 0;
	ct.wLength = 1;
	ct.timeout = 1000;
	ct.data = &val;

	rc = ioctl(fd, USBDEVFS_CONTROL, &ct);
	if (rc < 0) {
		msg("GET_CONFIGURATION failed (%s), assuming the first one",
		    strerror(errno));
		return 0;
	}
	return val;
}

static int probe_device(int fd, struct udev *d, const char *busid_override)
{
	unsigned char buf[MAX_DESC];
	struct usbdevfs_conninfo_ex cx;
	struct usbdevfs_connectinfo ci;
	ssize_t n;
	int conf, speed, i;

	/*
	 * pread, not read: the Java side may have read the descriptors already
	 * and left the file position past them, and this is somebody else's
	 * open file description.
	 */
	n = pread(fd, buf, sizeof(buf), 0);
	if (n < 0) {
		msg("reading descriptors: %s", strerror(errno));
		return -1;
	}

	memset(&cx, 0, sizeof(cx));
	cx.size = sizeof(cx);
	if (ioctl(fd, USBDEVFS_CONNINFO_EX(sizeof(cx)), &cx) == 0 && cx.size) {
		d->busnum = cx.busnum;
		d->devnum = cx.devnum;
		d->speed = cx.speed;
		/* busid is the physical path: bus-port[.port...] */
		if (cx.num_ports && cx.num_ports <= sizeof(cx.ports)) {
			int off = snprintf(d->busid, sizeof(d->busid), "%u-",
					   cx.busnum);

			for (i = 0; i < cx.num_ports && off > 0 &&
			     (size_t)off < sizeof(d->busid); i++)
				off += snprintf(d->busid + off,
						sizeof(d->busid) - off, "%s%u",
						i ? "." : "", cx.ports[i]);
		}
	} else {
		/*
		 * CONNINFO_EX arrived in 4.15. The old CONNINFO has no bus
		 * number at all, so bus 1 is asserted: devid only has to be
		 * consistent between this end and the guest's attach, and it
		 * is printed for exactly that reason.
		 */
		memset(&ci, 0, sizeof(ci));
		if (ioctl(fd, USBDEVFS_CONNECTINFO, &ci) < 0) {
			msg("USBDEVFS_CONNECTINFO: %s -- is fd %d really an "
			    "open /dev/bus/usb node?", strerror(errno), fd);
			return -1;
		}
		d->busnum = 1;
		d->devnum = ci.devnum;
		speed = ioctl(fd, USBDEVFS_GET_SPEED);
		d->speed = speed > 0 ? (uint32_t)speed : 0;
	}

	if (busid_override)
		snprintf(d->busid, sizeof(d->busid), "%s", busid_override);
	if (!d->busid[0])
		snprintf(d->busid, sizeof(d->busid), "%u-%u", d->busnum,
			 d->devnum);
	snprintf(d->path, sizeof(d->path), "/sys/bus/usb/devices/%s",
		 d->busid);

	conf = get_configuration(fd);
	if (parse_descriptors(buf, (size_t)n, conf, d))
		return -1;
	return 0;
}

static void dump_device(const struct udev *d)
{
	static const char *tname[] = { "iso", "int", "ctrl", "bulk" };
	int dir, ep, i;

	for (i = 0; i < d->nifs; i++)
		msg("  interface %u: class %02x/%02x/%02x", d->ifnum[i],
		    d->ifclass[i], d->ifsub[i], d->ifproto[i]);
	for (dir = 0; dir < 2; dir++)
		for (ep = 0; ep < 16; ep++)
			if (d->eptype[dir][ep] >= 0)
				msg("  endpoint %02x: %s",
				    ep | (dir == USBIP_DIR_IN ? USB_DIR_IN : 0),
				    tname[d->eptype[dir][ep] & 3]);
}

static const char *speed_str(uint32_t s)
{
	switch (s) {
	case 1: return "low";
	case 2: return "full";
	case 3: return "high";
	case 4: return "wireless";
	case 5: return "super";
	case 6: return "super+";
	default: return "unknown";
	}
}

/*
 * Claim every interface of the active configuration, disconnecting whatever
 * driver holds it. Without this the first URB fails with -ENODEV or -EBUSY:
 * usbfs checks the claim in checkintf() on submit, not on open.
 */
static int claim_interfaces(int fd, const struct udev *d)
{
	struct usbdevfs_disconnect_claim dc;
	int i;

	for (i = 0; i < d->nifs; i++) {
		memset(&dc, 0, sizeof(dc));
		dc.interface = d->ifnum[i];
		dc.flags = 0;		/* disconnect any driver, then claim */

		if (ioctl(fd, USBDEVFS_DISCONNECT_CLAIM, &dc) == 0)
			continue;

		/*
		 * Two failures are worth a second try with the plain claim,
		 * because both mean the Java side has already done the part
		 * that needs the privilege we lack:
		 *
		 *   EBUSY   the interface is claimed through this same file
		 *           description, i.e. UsbDeviceConnection.claimInterface
		 *           was called before the fd was handed over;
		 *   EACCES  privileges were dropped on this fd
		 *           (USBDEVFS_DROP_PRIVILEGES), which makes usbfs refuse
		 *           to disconnect a driver but still allows claiming an
		 *           interface that no driver holds.
		 *
		 * Anything else -- in particular a claim held by another
		 * process -- is fatal and has to be said out loud here, since
		 * every URB afterwards would fail the same anonymous way.
		 */
		if ((errno == EBUSY || errno == EACCES) &&
		    ioctl(fd, USBDEVFS_CLAIMINTERFACE, &dc.interface) == 0) {
			dbg("interface %u was already claimed on this fd",
			    dc.interface);
			continue;
		}

		msg("claiming interface %u: %s", dc.interface,
		    strerror(errno));
		return -1;
	}
	return 0;
}

static void release_interfaces(int fd, const struct udev *d)
{
	unsigned int ifnum;
	int i;

	for (i = 0; i < d->nifs; i++) {
		ifnum = d->ifnum[i];
		if (ioctl(fd, USBDEVFS_RELEASEINTERFACE, &ifnum) < 0 &&
		    errno != ENODEV)
			msg("releasing interface %u: %s", ifnum,
			    strerror(errno));
	}
}

/* ------------------------------------------------------------------ */
/* In-flight URBs.
 *
 * A WiFi adapter keeps several bulk IN URBs queued at all times, so the table
 * is a hash on seqnum rather than a single slot; a one-at-a-time server does
 * work, at one round trip per packet.
 */

#define REQ_BUCKETS	256

struct req {
	struct req *next;
	uint32_t seqnum;		/* CMD_SUBMIT seqnum, the wire key */
	uint32_t unlink_seqnum;		/* seqnum of the CMD_UNLINK, if any */
	int unlinking;
	int orphan;			/* client is gone; discard on reap */
	int dir_in;
	int ctrl;			/* buffer starts with the 8-byte setup */
	int np;				/* iso packets, 0 otherwise */
	struct usbip_iso_packet *iso;	/* client's descriptors, np entries */
	unsigned char *buf;
	size_t buflen;
	struct usbdevfs_urb urb;	/* iso_frame_desc[] follows */
};

static struct req *reqs[REQ_BUCKETS];
static unsigned int reqs_inflight;

static struct req *req_alloc(int np)
{
	struct req *r;
	size_t sz = sizeof(*r) + (size_t)np *
		sizeof(struct usbdevfs_iso_packet_desc);

	r = calloc(1, sz);
	if (!r)
		return NULL;
	if (np) {
		r->iso = calloc((size_t)np, sizeof(*r->iso));
		if (!r->iso) {
			free(r);
			return NULL;
		}
	}
	r->np = np;
	return r;
}

static void req_free(struct req *r)
{
	free(r->iso);
	free(r->buf);
	free(r);
}

static void req_insert(struct req *r)
{
	struct req **head = &reqs[r->seqnum % REQ_BUCKETS];

	r->next = *head;
	*head = r;
	reqs_inflight++;
}

static struct req *req_find(uint32_t seqnum)
{
	struct req *r;

	for (r = reqs[seqnum % REQ_BUCKETS]; r; r = r->next)
		if (r->seqnum == seqnum)
			return r;
	return NULL;
}

/* Is this pointer still one of ours? A reaped URB carries it back from the
 * kernel, and following a stale one would be the worst kind of bug to debug.
 */
static int req_is_live(const struct req *r)
{
	const struct req *p;

	for (p = reqs[r->seqnum % REQ_BUCKETS]; p; p = p->next)
		if (p == r)
			return 1;
	return 0;
}

static void req_unlink(struct req *r)
{
	struct req **p = &reqs[r->seqnum % REQ_BUCKETS];

	while (*p) {
		if (*p == r) {
			*p = r->next;
			reqs_inflight--;
			return;
		}
		p = &(*p)->next;
	}
}

/* ------------------------------------------------------------------ */
/* Replies. */

static int send_ret_submit(int sock, struct req *r)
{
	struct usbip_header h;
	struct usbip_iso_packet *iso = NULL;
	const unsigned char *data;
	int32_t actual = r->urb.actual_length;
	int i, rc = 0;

	memset(&h, 0, sizeof(h));
	h.command = USBIP_RET_SUBMIT;
	h.seqnum = r->seqnum;
	/* devid/direction/ep are the client's fields; a server sends zero. */

	/*
	 * usbfs hands back the URB status the USB core produced -- 0, or a
	 * negative errno such as -EPIPE for a stall or -EOVERFLOW for a babble
	 * -- which is precisely what the client wants to see in urb->status.
	 */
	h.u.ret_submit.status = r->urb.status;
	h.u.ret_submit.start_frame = r->urb.start_frame;
	h.u.ret_submit.error_count = r->urb.error_count;
	/*
	 * Zero for everything that is not isochronous: that is what the
	 * in-kernel server sends (urb->number_of_packets of a non-iso URB),
	 * and the client clamps anything negative to zero anyway.
	 */
	h.u.ret_submit.number_of_packets = r->np;

	if (r->np > 0) {
		/*
		 * For isochronous IN the client checks that actual_length is
		 * the sum of the per-packet actual lengths and drops the
		 * connection if it is not, so sum them here rather than trust
		 * urb.actual_length, which the host controller drivers do not
		 * all fill in for iso.
		 */
		actual = 0;
		for (i = 0; i < r->np; i++)
			actual += r->urb.iso_frame_desc[i].actual_length;
	}
	h.u.ret_submit.actual_length = actual;

	hdr_hton(&h);
	if (write_full(sock, &h, sizeof(h)))
		return -1;

	/* Payload: IN data only, and for iso packed with the padding removed */
	if (r->dir_in && actual > 0 && !r->np) {
		data = r->ctrl ? r->buf + 8 : r->buf;
		if (write_full(sock, data, (size_t)actual))
			return -1;
	} else if (r->dir_in && r->np > 0) {
		size_t off = 0;

		for (i = 0; i < r->np; i++) {
			size_t len = r->urb.iso_frame_desc[i].actual_length;

			if (write_full(sock, r->buf + off, len))
				return -1;
			off += r->urb.iso_frame_desc[i].length;
		}
	}

	if (r->np > 0) {
		iso = calloc((size_t)r->np, sizeof(*iso));
		if (!iso)
			return -1;
		for (i = 0; i < r->np; i++) {
			/*
			 * offset and length are echoed from the request: the
			 * client uses them to put the padding back. Only the
			 * results come from usbfs.
			 */
			iso[i].offset = htonl(r->iso[i].offset);
			iso[i].length = htonl(r->iso[i].length);
			iso[i].actual_length =
				htonl(r->urb.iso_frame_desc[i].actual_length);
			iso[i].status =
				htonl(r->urb.iso_frame_desc[i].status);
		}
		rc = write_full(sock, iso, (size_t)r->np * sizeof(*iso));
		free(iso);
	}
	return rc;
}

/* A completion the device never saw: submission itself failed. */
static int send_ret_submit_err(int sock, uint32_t seqnum, int32_t status)
{
	struct usbip_header h;

	memset(&h, 0, sizeof(h));
	h.command = USBIP_RET_SUBMIT;
	h.seqnum = seqnum;
	h.u.ret_submit.status = status;
	hdr_hton(&h);
	return write_full(sock, &h, sizeof(h));
}

static int send_ret_unlink(int sock, uint32_t seqnum, int32_t status)
{
	struct usbip_header h;

	memset(&h, 0, sizeof(h));
	h.command = USBIP_RET_UNLINK;
	h.seqnum = seqnum;		/* the UNLINK's seqnum, not the URB's */
	h.u.ret_unlink.status = status;
	hdr_hton(&h);
	return write_full(sock, &h, sizeof(h));
}

/* ------------------------------------------------------------------ */
/* CMD_SUBMIT -> USBDEVFS_SUBMITURB. */

static unsigned int map_flags(uint32_t f, int type)
{
	unsigned int u = 0;

	if (f & USBIP_URB_SHORT_NOT_OK)
		u |= USBDEVFS_URB_SHORT_NOT_OK;
	if (f & USBIP_URB_ZERO_PACKET)
		u |= USBDEVFS_URB_ZERO_PACKET;
	if (f & USBIP_URB_NO_INTERRUPT)
		u |= USBDEVFS_URB_NO_INTERRUPT;
	/*
	 * ISO_ASAP is in usbfs's accepted mask only for iso URBs -- devio.c
	 * rejects the whole submission with -EINVAL for any flag outside the
	 * mask -- so it is filtered by type here. SHORT_NOT_OK and
	 * ZERO_PACKET on a direction where they make no sense are merely
	 * warned about and dropped, so those are passed through as the client
	 * sent them.
	 */
	if (type == USBDEVFS_URB_TYPE_ISO && (f & USBIP_URB_ISO_ASAP))
		u |= USBDEVFS_URB_ISO_ASAP;
	return u;
}

/*
 * Fill in the usbfs URB from a decoded CMD_SUBMIT. r->buf is already
 * allocated and holds the OUT payload (at r->buf + 8 for control, where the
 * setup packet goes first, which is how usbfs wants a control URB: the 8-byte
 * setup, then the data, with buffer_length covering both).
 *
 * Returns 0, or a negative errno to answer the client with.
 */
static int urb_from_pdu(struct req *r, const struct usbip_header *h,
			const struct udev *d)
{
	int epnum = h->ep & 0x0f;
	int dir = r->dir_in ? USBIP_DIR_IN : USBIP_DIR_OUT;
	int type, i;

	if (h->ep > 15)
		return -EINVAL;

	if (epnum == 0) {
		type = USBDEVFS_URB_TYPE_CONTROL;
	} else {
		type = d->eptype[dir][epnum];
		if (type < 0) {
			msg("seq %u: no endpoint %d %s in configuration %u",
			    h->seqnum, epnum, r->dir_in ? "IN" : "OUT",
			    d->confval);
			return -EPIPE;
		}
	}

	if ((type == USBDEVFS_URB_TYPE_ISO) != (r->np > 0)) {
		msg("seq %u: %d iso packets on a non-iso endpoint %d (or "
		    "none on an iso one)", h->seqnum, r->np, epnum);
		return -EINVAL;
	}

	r->urb.type = type;
	/* usbfs wants the endpoint *address*, direction bit included. */
	r->urb.endpoint = epnum | (r->dir_in ? USB_DIR_IN : 0);
	r->urb.flags = map_flags(h->u.cmd_submit.transfer_flags, type);
	r->urb.buffer = r->buf;
	r->urb.buffer_length = (int)r->buflen;
	r->urb.start_frame = h->u.cmd_submit.start_frame;
	r->urb.number_of_packets = r->np;
	r->urb.usercontext = r;
	/*
	 * The interval is not passed on: usbfs computes it from the endpoint
	 * descriptor and struct usbdevfs_urb has nowhere to put one.
	 */

	if (type == USBDEVFS_URB_TYPE_CONTROL) {
		memcpy(r->buf, h->u.cmd_submit.setup, 8);
		/*
		 * usbfs takes the direction of a control transfer from the
		 * setup packet, not from the endpoint address, and treats a
		 * zero-length IN request as OUT (devio.c, USBDEVFS_URB_TYPE_
		 * CONTROL). Follow it exactly, so that what is sent back is
		 * what usbfs actually transferred: the client derives its own
		 * pipe direction from the same two bytes, so the two agree.
		 */
		r->dir_in = (h->u.cmd_submit.setup[0] & USB_DIR_IN) &&
			    get16(h->u.cmd_submit.setup + 6) > 0;
		r->urb.endpoint = r->dir_in ? USB_DIR_IN : 0;
	}

	for (i = 0; i < r->np; i++)
		r->urb.iso_frame_desc[i].length = r->iso[i].length;

	return 0;
}

/*
 * Four control requests must not be sent down usbfs as raw control
 * transfers, exactly as the in-kernel stub tweaks them: they change state the
 * host kernel is tracking, and doing them behind its back leaves usbfs
 * rejecting every following URB because its idea of the active configuration
 * or altsetting no longer matches the device's.
 *
 * Returns 1 if handled (with *status set), 0 if this is an ordinary transfer.
 */
static int tweak_control(int fd, struct udev *d, const unsigned char *setup,
			 int *status)
{
	unsigned int type = setup[0], req = setup[1];
	unsigned int value = get16(setup + 2), index = get16(setup + 4);
	struct usbdevfs_setinterface si;
	unsigned int arg;
	int rc = 0;

	if (type == 0x00 && req == USB_REQ_SET_CONFIG) {
		if (value == d->confval) {
			/*
			 * The guest's first enumeration sets the config the
			 * device is already in. usbfs would refuse it anyway:
			 * proc_setconfig() returns -EBUSY when any interface
			 * is claimed, and we hold every one of them.
			 */
			dbg("SET_CONFIGURATION %u: already active", value);
			*status = 0;
			return 1;
		}
		release_interfaces(fd, d);
		arg = value;
		if (ioctl(fd, USBDEVFS_SETCONFIGURATION, &arg) < 0) {
			rc = -errno;
			msg("SET_CONFIGURATION %u: %s", value,
			    strerror(errno));
		} else {
			unsigned char buf[MAX_DESC];
			ssize_t n = pread(fd, buf, sizeof(buf), 0);

			if (n < 0 || parse_descriptors(buf, (size_t)n,
						       (int)value, d))
				rc = -EIO;
		}
		if (claim_interfaces(fd, d) && !rc)
			rc = -EBUSY;
		*status = rc;
		return 1;
	}

	if (type == 0x01 && req == USB_REQ_SET_INTERFACE) {
		memset(&si, 0, sizeof(si));
		si.interface = index;
		si.altsetting = value;
		if (ioctl(fd, USBDEVFS_SETINTERFACE, &si) < 0) {
			rc = -errno;
			msg("SET_INTERFACE %u/%u: %s", index, value,
			    strerror(errno));
		}
		*status = rc;
		return 1;
	}

	/* CLEAR_FEATURE(ENDPOINT_HALT): usbfs also resets the data toggle. */
	if (type == 0x02 && req == USB_REQ_CLEAR_FEATURE && value == 0) {
		arg = index;
		if (ioctl(fd, USBDEVFS_CLEAR_HALT, &arg) < 0) {
			rc = -errno;
			msg("CLEAR_HALT ep %02x: %s", index, strerror(errno));
		}
		*status = rc;
		return 1;
	}

	/* SET_FEATURE(PORT_RESET) on the hub port -- a device reset. */
	if (type == 0x23 && req == USB_REQ_SET_FEATURE && value == 4) {
		if (ioctl(fd, USBDEVFS_RESET) < 0) {
			rc = -errno;
			msg("USBDEVFS_RESET: %s", strerror(errno));
		}
		*status = rc;
		return 1;
	}

	return 0;
}

/*
 * Read one CMD_SUBMIT payload and submit it. Returns 0 to keep serving, -1 if
 * the stream can no longer be trusted.
 *
 * The payload is always consumed, even for a request that cannot be honoured:
 * the header carries the only length information there is, so leaving those
 * bytes in the socket turns one bad URB into a permanently desynchronised
 * connection.
 */
static int handle_submit(int sock, struct usbip_header *h)
{
	int32_t xlen = h->u.cmd_submit.transfer_buffer_length;
	int32_t np = h->u.cmd_submit.number_of_packets;
	int dir_in = h->direction == USBIP_DIR_IN;
	size_t isosz, i, off;
	unsigned char *wire = NULL;
	struct req *r;
	int status, rc;

	if (xlen < 0 || xlen > MAX_XFER) {
		msg("seq %u: transfer_buffer_length %d out of range -- "
		    "the stream is out of sync, giving up", h->seqnum, xlen);
		return -1;
	}
	/*
	 * The client sends 0 here for everything that is not isochronous
	 * (vhci_tx packs urb->number_of_packets straight through), so np > 0
	 * is the only signal that iso descriptors follow the buffer.
	 */
	if (np < 0 || np > MAX_ISO_PACKETS) {
		msg("seq %u: number_of_packets %d out of range -- "
		    "the stream is out of sync, giving up", h->seqnum, np);
		return -1;
	}

	isosz = (size_t)np * sizeof(struct usbip_iso_packet);

	r = req_alloc(np);
	if (!r) {
		size_t skip = (dir_in ? 0 : (size_t)xlen) + isosz;

		msg("seq %u: out of memory for %d iso packets", h->seqnum, np);
		if (skip_full(sock, skip) <= 0)
			return -1;
		return send_ret_submit_err(sock, h->seqnum, -ENOMEM);
	}
	r->seqnum = h->seqnum;
	r->dir_in = dir_in;
	r->ctrl = (h->ep & 0x0f) == 0;

	/*
	 * Wire order after the header: the transfer buffer, for an OUT
	 * transfer only, and then the isochronous descriptors if there are
	 * any. Both have to be consumed before the next header is anywhere.
	 */
	if (!dir_in && xlen > 0) {
		wire = malloc((size_t)xlen);
		if (!wire || read_full(sock, wire, (size_t)xlen) <= 0) {
			msg("seq %u: short read of %d OUT bytes", h->seqnum,
			    xlen);
			free(wire);
			req_free(r);
			return -1;
		}
	}
	if (np) {
		struct usbip_iso_packet *iso = r->iso;

		if (read_full(sock, iso, isosz) <= 0) {
			msg("seq %u: short read of %zu iso bytes", h->seqnum,
			    isosz);
			free(wire);
			req_free(r);
			return -1;
		}
		for (i = 0; i < (size_t)np; i++) {
			iso[i].offset = ntohl(iso[i].offset);
			iso[i].length = ntohl(iso[i].length);
			iso[i].actual_length = ntohl(iso[i].actual_length);
			iso[i].status = ntohl(iso[i].status);
		}
	}

	/*
	 * Buffer layout towards usbfs:
	 *   control  8-byte setup packet, then wLength bytes of data
	 *   iso      the packets back to back, no padding: usbfs computes the
	 *            frame offsets itself by summing the lengths, so a client
	 *            buffer with gaps has to be repacked here
	 *   other    the data as it came off the wire
	 */
	if (np) {
		r->buflen = 0;
		for (i = 0; i < (size_t)np; i++)
			r->buflen += r->iso[i].length;
	} else {
		r->buflen = (size_t)xlen + (r->ctrl ? 8 : 0);
	}

	r->buf = calloc(1, r->buflen ? r->buflen : 1);
	if (!r->buf) {
		msg("seq %u: out of memory for %zu bytes", h->seqnum,
		    r->buflen);
		status = -ENOMEM;
		goto fail;
	}

	if (wire) {
		if (np) {
			for (i = 0, off = 0; i < (size_t)np; i++) {
				size_t o = r->iso[i].offset;
				size_t l = r->iso[i].length;

				if (o + l > (size_t)xlen)
					break;
				memcpy(r->buf + off, wire + o, l);
				off += l;
			}
			if (i != (size_t)np) {
				msg("seq %u: iso packet %zu runs past the "
				    "%d-byte buffer", h->seqnum, i, xlen);
				status = -EINVAL;
				goto fail;
			}
		} else {
			memcpy(r->buf + (r->ctrl ? 8 : 0), wire,
			       (size_t)xlen);
		}
		free(wire);
		wire = NULL;
	}

	status = urb_from_pdu(r, h, &dev);
	if (status)
		goto fail;

	if (r->urb.type == USBDEVFS_URB_TYPE_CONTROL &&
	    tweak_control(usbfd, &dev, r->buf, &status)) {
		r->urb.status = status;
		r->urb.actual_length = 0;
		rc = send_ret_submit(sock, r);
		req_free(r);
		return rc;
	}

	dbg("submit seq %u ep %u %s type %d len %zu", r->seqnum, h->ep & 0x0f,
	    dir_in ? "IN" : "OUT", r->urb.type, r->buflen);

	req_insert(r);
	if (ioctl(usbfd, USBDEVFS_SUBMITURB, &r->urb) < 0) {
		status = -errno;
		msg("seq %u: SUBMITURB ep %u %s type %d len %zu: %s",
		    r->seqnum, h->ep & 0x0f, dir_in ? "IN" : "OUT",
		    r->urb.type, r->buflen, strerror(errno));
		req_unlink(r);
		goto fail;
	}
	return 0;

fail:
	free(wire);
	req_free(r);
	return send_ret_submit_err(sock, h->seqnum, status);
}

/*
 * CMD_UNLINK. The reply carries the *unlink* request's seqnum, and the URB
 * being cancelled gets no RET_SUBMIT at all -- vhci_rx looks up the pending
 * unlink by that seqnum and gives the URB back itself.
 */
static int handle_unlink(int sock, struct usbip_header *h)
{
	uint32_t target = h->u.cmd_unlink.seqnum;
	struct req *r = req_find(target);

	if (!r) {
		/* Already completed and answered: nothing to cancel. */
		dbg("unlink seq %u: %u not pending", h->seqnum, target);
		return send_ret_unlink(sock, h->seqnum, 0);
	}

	r->unlinking = 1;
	r->unlink_seqnum = h->seqnum;

	/*
	 * DISCARDURB fails with EINVAL when the URB has already completed but
	 * not yet been reaped. That is not an error: the completion is on its
	 * way and the reap path will send the RET_UNLINK, because the request
	 * is marked either way.
	 */
	if (ioctl(usbfd, USBDEVFS_DISCARDURB, &r->urb) < 0 && errno != EINVAL)
		msg("unlink seq %u: DISCARDURB on %u: %s", h->seqnum, target,
		    strerror(errno));
	return 0;
}

/* Drain every completion usbfs has ready. */
static int reap_completions(int sock)
{
	struct usbdevfs_urb *u;
	struct req *r;

	for (;;) {
		u = NULL;
		if (ioctl(usbfd, USBDEVFS_REAPURBNDELAY, &u) < 0) {
			if (errno == EAGAIN || errno == EINTR)
				return 0;
			if (errno == ENODEV) {
				msg("device disconnected while reaping");
				return -1;
			}
			msg("REAPURBNDELAY: %s", strerror(errno));
			return -1;
		}
		if (!u)
			continue;

		r = u->usercontext;
		if (!r || !req_is_live(r)) {
			msg("reaped an URB that is not in the table (%p)",
			    (void *)u);
			return -1;
		}

		if (u->status)
			dbg("seq %u completed: status %d actual %d",
			    r->seqnum, u->status, u->actual_length);

		req_unlink(r);
		if (r->orphan) {
			/* Left over from a client that is no longer here. */
			req_free(r);
			continue;
		}
		if (r->unlinking) {
			int rc = send_ret_unlink(sock, r->unlink_seqnum,
						u->status);

			req_free(r);
			if (rc)
				return -1;
		} else {
			int rc = send_ret_submit(sock, r);

			req_free(r);
			if (rc)
				return -1;
		}
	}
}

/*
 * Cancel and clean up everything still in flight, in that order: cancel, then
 * reap, then free.
 *
 * The order is not a style choice. usbfs copies an IN transfer's data into
 * the submitting process's buffer when the URB is *reaped*, not when it
 * completes (devio.c's processcompl, as->userbuffer), so freeing a request
 * before its completion has been reaped leaves the kernel a dangling pointer
 * to copy into. Discarding is asynchronous too, which is why the reap is
 * given a bounded number of chances to catch up rather than one pass.
 */
static void discard_all(void)
{
	struct req *r, *next;
	int round, i;

	for (i = 0; i < REQ_BUCKETS; i++)
		for (r = reqs[i]; r; r = r->next)
			ioctl(usbfd, USBDEVFS_DISCARDURB, &r->urb);

	for (round = 0; reqs_inflight && round < 20; round++) {
		struct pollfd p = { .fd = usbfd, .events = POLLOUT };
		struct usbdevfs_urb *u = NULL;

		if (ioctl(usbfd, USBDEVFS_REAPURBNDELAY, &u) < 0) {
			if (errno != EAGAIN && errno != EINTR)
				break;
			poll(&p, 1, 50);
			continue;
		}
		if (u && u->usercontext) {
			r = u->usercontext;
			if (req_is_live(r)) {
				req_unlink(r);
				req_free(r);
			}
		}
	}

	/*
	 * Whatever is left is still owned by the kernel. Keep it in the table
	 * and mark it, so that the reap path recognises it if it ever does
	 * come back -- freeing it here would be the dangling pointer above,
	 * and dropping it from the table would make that late completion look
	 * like a protocol error to the next client.
	 */
	if (reqs_inflight) {
		msg("%u URBs never came back; their buffers stay allocated "
		    "until usbfs is done with them", reqs_inflight);
		for (i = 0; i < REQ_BUCKETS; i++)
			for (r = reqs[i]; r; r = next) {
				next = r->next;
				r->orphan = 1;
			}
	}
}

/* ------------------------------------------------------------------ */
/* The op phase: OP_REQ_DEVLIST / OP_REQ_IMPORT. */

static void fill_usb_device(struct usbip_usb_device *ud, const struct udev *d)
{
	memset(ud, 0, sizeof(*ud));
	snprintf(ud->path, sizeof(ud->path), "%s", d->path);
	snprintf(ud->busid, sizeof(ud->busid), "%s", d->busid);
	ud->busnum = htonl(d->busnum);
	ud->devnum = htonl(d->devnum);
	ud->speed = htonl(d->speed);
	ud->idVendor = htons(d->vid);
	ud->idProduct = htons(d->pid);
	ud->bcdDevice = htons(d->bcd);
	ud->bDeviceClass = d->dclass;
	ud->bDeviceSubClass = d->dsub;
	ud->bDeviceProtocol = d->dproto;
	ud->bConfigurationValue = d->confval;
	ud->bNumConfigurations = d->nconfigs;
	ud->bNumInterfaces = d->nifs;
}

static int send_op_common(int sock, uint16_t code, uint32_t status)
{
	struct op_common op;

	op.version = htons(USBIP_VERSION);
	op.code = htons(code);
	op.status = htonl(status);
	return write_full(sock, &op, sizeof(op));
}

static int send_devlist(int sock, const struct udev *d)
{
	struct usbip_usb_device ud;
	struct usbip_usb_interface ui;
	uint32_t ndev = htonl(1);
	int i;

	if (send_op_common(sock, OP_REP_DEVLIST, ST_OK) ||
	    write_full(sock, &ndev, sizeof(ndev)))
		return -1;

	fill_usb_device(&ud, d);
	if (write_full(sock, &ud, sizeof(ud)))
		return -1;

	for (i = 0; i < d->nifs; i++) {
		ui.bInterfaceClass = d->ifclass[i];
		ui.bInterfaceSubClass = d->ifsub[i];
		ui.bInterfaceProtocol = d->ifproto[i];
		ui.padding = 0;
		if (write_full(sock, &ui, sizeof(ui)))
			return -1;
	}
	return 0;
}

static int send_import_reply(int sock, const struct udev *d)
{
	struct usbip_usb_device ud;

	/*
	 * OP_REP_IMPORT carries the device block and no interfaces -- the
	 * client is about to enumerate the device itself over the same
	 * connection.
	 */
	if (send_op_common(sock, OP_REP_IMPORT, ST_OK))
		return -1;
	fill_usb_device(&ud, d);
	return write_full(sock, &ud, sizeof(ud));
}

/*
 * Returns 1 when the client asked to import (URB traffic follows), 0 to keep
 * reading op requests, -1 on error.
 */
static int handle_op(int sock, const struct udev *d)
{
	char busid[SYSFS_BUS_ID_SIZE];
	struct op_common op;
	uint16_t code;
	int rc;

	rc = read_full(sock, &op, sizeof(op));
	/*
	 * EOF here is the normal end of a devlist query: the reference client
	 * asks, reads the answer and closes. Only a short or failed read is
	 * an error worth a nonzero exit.
	 */
	if (rc == 0)
		return -2;
	if (rc < 0)
		return -1;

	if (ntohs(op.version) != USBIP_VERSION)
		msg("client speaks usbip version %04x, this is %04x -- "
		    "continuing anyway", ntohs(op.version), USBIP_VERSION);

	code = ntohs(op.code);
	switch (code) {
	case OP_REQ_DEVLIST:
		dbg("OP_REQ_DEVLIST");
		if (send_devlist(sock, d))
			return -1;
		return 0;
	case OP_REQ_IMPORT:
		if (read_full(sock, busid, sizeof(busid)) <= 0)
			return -1;
		busid[sizeof(busid) - 1] = '\0';
		dbg("OP_REQ_IMPORT %s", busid);
		if (strcmp(busid, d->busid)) {
			msg("import request for busid %s, this is %s", busid,
			    d->busid);
			send_op_common(sock, OP_REP_IMPORT, ST_NA);
			return -1;
		}
		if (send_import_reply(sock, d))
			return -1;
		return 1;
	default:
		msg("unexpected op code %04x", code);
		send_op_common(sock, code & 0x7fff, ST_NA);
		return -1;
	}
}

/*
 * A client that already knows what it is attaching to may skip the op phase
 * entirely and start submitting URBs -- the guest side can, since the devid
 * and speed are printed here and the connection is point to point. The two
 * cases are told apart by the first two bytes: an op request starts with the
 * version, 0x01 0x11, while a URB header starts with a big-endian command
 * number, so byte 0 is always 0x00.
 */
static int peek_is_op(int sock)
{
	unsigned char b[2];
	ssize_t n;

	for (;;) {
		n = recv(sock, b, sizeof(b), MSG_PEEK);
		if (n < 0 && errno == EINTR)
			continue;
		if (n <= 0)
			return -1;
		if (n < (ssize_t)sizeof(b)) {
			/* Wait for the second byte rather than guess. */
			struct pollfd p = { .fd = sock, .events = POLLIN };

			if (poll(&p, 1, -1) < 0 && errno != EINTR)
				return -1;
			continue;
		}
		return b[0] == (USBIP_VERSION >> 8) &&
		       b[1] == (USBIP_VERSION & 0xff);
	}
}

/* ------------------------------------------------------------------ */

static int serve(int sock)
{
	struct pollfd pfd[2];
	int importing;

	importing = peek_is_op(sock);
	if (importing < 0) {
		/* Somebody connected and went away: not this program's fault */
		msg("client closed before saying anything");
		return 0;
	}
	while (importing) {
		int rc = handle_op(sock, &dev);

		if (rc == -2)
			return 0;
		if (rc < 0)
			return -1;
		if (rc > 0)
			break;
	}
	msg("serving %04x:%04x on busid %s", dev.vid, dev.pid, dev.busid);

	for (;;) {
		struct usbip_header h;
		int rc;

		pfd[0].fd = sock;
		pfd[0].events = POLLIN;
		/*
		 * POLLOUT, not POLLIN: usbfs reports a ready completion by
		 * making the descriptor writable (devio.c's usbdev_poll), and
		 * a usbfs fd is never readable in the poll sense at all.
		 */
		pfd[1].fd = usbfd;
		pfd[1].events = POLLOUT;

		if (poll(pfd, 2, -1) < 0) {
			if (errno == EINTR)
				continue;
			msg("poll: %s", strerror(errno));
			return -1;
		}

		if (pfd[1].revents & POLLOUT) {
			if (reap_completions(sock))
				return -1;
		}
		if (pfd[1].revents & (POLLERR | POLLHUP)) {
			msg("usbfs fd %d reports %s -- device unplugged?",
			    usbfd,
			    (pfd[1].revents & POLLHUP) ? "HUP" : "ERR");
			return -1;
		}

		if (pfd[0].revents & POLLIN) {
			rc = read_full(sock, &h, sizeof(h));
			if (rc == 0) {
				msg("client detached (%u URBs in flight)",
				    reqs_inflight);
				return 0;
			}
			if (rc < 0) {
				msg("reading a pdu: %s", strerror(errno));
				return -1;
			}
			hdr_ntoh(&h);

			switch (h.command) {
			case USBIP_CMD_SUBMIT:
				if (handle_submit(sock, &h))
					return -1;
				break;
			case USBIP_CMD_UNLINK:
				if (handle_unlink(sock, &h))
					return -1;
				break;
			default:
				msg("unknown command %u seq %u -- dropping "
				    "the connection", h.command, h.seqnum);
				return -1;
			}
		} else if (pfd[0].revents & (POLLERR | POLLHUP)) {
			msg("client hung up (%u URBs in flight)",
			    reqs_inflight);
			return 0;
		}
	}
}

/* ------------------------------------------------------------------ */
/* Self-test: the wire vectors in Documentation/usb/usbip_protocol.rst, and a
 * synthetic descriptor set for the adapter this exists for. Byte for byte,
 * because a field-order or endianness mistake is otherwise only visible as a
 * device that enumerates and then does nothing.
 */

static int failures;

static void check(int ok, const char *what)
{
	if (!ok) {
		failures++;
		printf("FAIL %s\n", what);
	} else if (verbose) {
		printf("ok   %s\n", what);
	}
}

static void check_mem(const void *got, const void *want, size_t n,
		      const char *what)
{
	size_t i;

	if (!memcmp(got, want, n)) {
		if (verbose)
			printf("ok   %s\n", what);
		return;
	}
	failures++;
	printf("FAIL %s\n     got ", what);
	for (i = 0; i < n; i++)
		printf("%02x", ((const unsigned char *)got)[i]);
	printf("\n    want ");
	for (i = 0; i < n; i++)
		printf("%02x", ((const unsigned char *)want)[i]);
	printf("\n");
}

/* Big-endian 32-bit load from an arbitrary offset in a byte buffer. */
static uint32_t be32at(const unsigned char *p)
{
	uint32_t v;

	memcpy(&v, p, sizeof(v));
	return ntohl(v);
}

/* "00000001 00000d05 ..." -> bytes */
static size_t unhex(const char *s, unsigned char *out, size_t max)
{
	size_t n = 0;
	int hi = -1;

	for (; *s; s++) {
		int v;

		if (*s == ' ')
			continue;
		v = (*s >= '0' && *s <= '9') ? *s - '0' :
		    (*s >= 'a' && *s <= 'f') ? *s - 'a' + 10 : -1;
		if (v < 0)
			continue;
		if (hi < 0) {
			hi = v;
		} else {
			if (n < max)
				out[n++] = (unsigned char)((hi << 4) | v);
			hi = -1;
		}
	}
	return n;
}

/* The interrupt IN/OUT exchange captured in the protocol document. */
static const char *cmd_intr_in =
	"00000001 00000d05 0001000f 00000001 00000001"
	"00000200 00000040 ffffffff 00000000 00000004"
	"00000000 00000000";
static const char *cmd_intr_out =
	"00000001 00000d06 0001000f 00000000 00000001"
	"00000000 00000040 ffffffff 00000000 00000004"
	"00000000 00000000";
static const char *ret_intr_out =
	"00000003 00000d06 00000000 00000000 00000000"
	"00000000 00000040 ffffffff 00000000 00000000"
	"00000000 00000000";
static const char *ret_intr_in =
	"00000003 00000d05 00000000 00000000 00000000"
	"00000000 00000040 ffffffff 00000000 00000000"
	"00000000 00000000";

/*
 * A minimal RTL8811AU: one configuration, one vendor-specific interface, a
 * bulk IN at 0x84 and three bulk OUTs, which is what an 8821au presents.
 */
static const unsigned char rtl_desc[] = {
	/* device descriptor, 18 bytes */
	0x12, 0x01, 0x10, 0x02, 0xff, 0xff, 0xff, 0x40,
	0x57, 0x23, 0x1f, 0x01, 0x00, 0x02, 0x01, 0x02,
	0x03, 0x01,
	/* configuration descriptor, wTotalLength 46 */
	0x09, 0x02, 0x2e, 0x00, 0x01, 0x01, 0x00, 0x80, 0xfa,
	/* interface 0 alt 0, 4 endpoints, vendor specific */
	0x09, 0x04, 0x00, 0x00, 0x04, 0xff, 0xff, 0xff, 0x00,
	/* ep 0x84 bulk in, 0x02/0x03/0x04 bulk out */
	0x07, 0x05, 0x84, 0x02, 0x00, 0x02, 0x00,
	0x07, 0x05, 0x02, 0x02, 0x00, 0x02, 0x00,
	0x07, 0x05, 0x03, 0x02, 0x00, 0x02, 0x00,
	0x07, 0x05, 0x04, 0x02, 0x00, 0x02, 0x00,
};

static void test_layout(void)
{
	check(sizeof(struct usbip_header) == 48, "header is 48 bytes");
	check(offsetof(struct usbip_header, u) == 20, "union at 0x14");
	check(offsetof(struct usbip_header, u.cmd_submit.transfer_buffer_length)
	      == 0x18, "transfer_buffer_length at 0x18");
	check(offsetof(struct usbip_header, u.cmd_submit.number_of_packets)
	      == 0x20, "number_of_packets at 0x20");
	check(offsetof(struct usbip_header, u.cmd_submit.interval) == 0x24,
	      "interval at 0x24");
	check(offsetof(struct usbip_header, u.cmd_submit.setup) == 0x28,
	      "setup at 0x28");
	check(offsetof(struct usbip_header, u.ret_submit.actual_length) == 0x18,
	      "actual_length at 0x18");
	check(offsetof(struct usbip_usb_device, busnum) == 0x120,
	      "busnum at 0x120 of the device block");
	check(offsetof(struct usbip_usb_device, idVendor) == 0x12c,
	      "idVendor at 0x12c of the device block");
	check(sizeof(struct usbip_usb_device) == 312, "device block is 312");
}

static void test_cmd_submit_decode(void)
{
	struct usbip_header h;
	unsigned char raw[48];

	check(unhex(cmd_intr_in, raw, sizeof(raw)) == 48, "vector is 48 bytes");
	memcpy(&h, raw, sizeof(h));
	hdr_ntoh(&h);
	check(h.command == USBIP_CMD_SUBMIT, "CmdIntrIN: command");
	check(h.seqnum == 0xd05, "CmdIntrIN: seqnum");
	check(h.devid == 0x1000f, "CmdIntrIN: devid");
	check(h.direction == USBIP_DIR_IN, "CmdIntrIN: direction");
	check(h.ep == 1, "CmdIntrIN: ep");
	check(h.u.cmd_submit.transfer_flags == 0x200,
	      "CmdIntrIN: transfer_flags (URB_DIR_IN)");
	check(h.u.cmd_submit.transfer_buffer_length == 0x40,
	      "CmdIntrIN: transfer_buffer_length");
	check(h.u.cmd_submit.start_frame == -1, "CmdIntrIN: start_frame");
	check(h.u.cmd_submit.number_of_packets == 0,
	      "CmdIntrIN: number_of_packets");
	check(h.u.cmd_submit.interval == 4, "CmdIntrIN: interval");

	unhex(cmd_intr_out, raw, sizeof(raw));
	memcpy(&h, raw, sizeof(h));
	hdr_ntoh(&h);
	check(h.seqnum == 0xd06, "CmdIntrOUT: seqnum");
	check(h.direction == USBIP_DIR_OUT, "CmdIntrOUT: direction");
	check(h.u.cmd_submit.transfer_flags == 0, "CmdIntrOUT: flags");
}

static void test_ret_submit_encode(void)
{
	unsigned char want[48], got[48];
	struct usbip_header h;
	struct req r;

	/* The IN reply, built the way reap_completions() builds it. */
	memset(&r, 0, sizeof(r));
	r.seqnum = 0xd05;
	r.dir_in = 1;
	r.urb.status = 0;
	r.urb.actual_length = 0x40;
	r.urb.start_frame = -1;

	memset(&h, 0, sizeof(h));
	h.command = USBIP_RET_SUBMIT;
	h.seqnum = r.seqnum;
	h.u.ret_submit.status = r.urb.status;
	h.u.ret_submit.actual_length = r.urb.actual_length;
	h.u.ret_submit.start_frame = r.urb.start_frame;
	h.u.ret_submit.number_of_packets = 0;
	h.u.ret_submit.error_count = 0;
	hdr_hton(&h);
	memcpy(got, &h, sizeof(got));
	unhex(ret_intr_in, want, sizeof(want));
	check_mem(got, want, sizeof(want), "RetIntrIn encodes byte for byte");

	memset(&h, 0, sizeof(h));
	h.command = USBIP_RET_SUBMIT;
	h.seqnum = 0xd06;
	h.u.ret_submit.actual_length = 0x40;
	h.u.ret_submit.start_frame = -1;
	hdr_hton(&h);
	memcpy(got, &h, sizeof(got));
	unhex(ret_intr_out, want, sizeof(want));
	check_mem(got, want, sizeof(want), "RetIntrOut encodes byte for byte");
}

static void test_unlink(void)
{
	unsigned char raw[48];
	struct usbip_header h;

	/* CMD_UNLINK of seqnum 0x0d05, itself seqnum 0x0d07. */
	memset(&h, 0, sizeof(h));
	h.command = USBIP_CMD_UNLINK;
	h.seqnum = 0xd07;
	h.devid = 0x1000f;
	h.u.cmd_unlink.seqnum = 0xd05;
	hdr_hton(&h);
	memcpy(raw, &h, sizeof(raw));
	check(raw[3] == 0x02, "CMD_UNLINK command byte");
	check(raw[0x14] == 0 && raw[0x17] == 0x05,
	      "unlink_seqnum at 0x14, big endian");

	memcpy(&h, raw, sizeof(h));
	hdr_ntoh(&h);
	check(h.u.cmd_unlink.seqnum == 0xd05, "CMD_UNLINK round trip");

	/* RET_UNLINK carries -ECONNRESET when the URB really was cancelled. */
	memset(&h, 0, sizeof(h));
	h.command = USBIP_RET_UNLINK;
	h.seqnum = 0xd07;
	h.u.ret_unlink.status = -ECONNRESET;
	hdr_hton(&h);
	memcpy(raw, &h, sizeof(raw));
	check(raw[3] == 0x04, "RET_UNLINK command byte");
	check(be32at(&raw[0x14]) == (uint32_t)-ECONNRESET,
	      "RET_UNLINK status is -ECONNRESET");
}

static void test_descriptors(void)
{
	struct udev d;
	int i;

	memset(&d, 0, sizeof(d));
	check(parse_descriptors(rtl_desc, sizeof(rtl_desc), 0, &d) == 0,
	      "8811au descriptors parse");
	check(d.vid == 0x2357 && d.pid == 0x011f, "vid:pid 2357:011f");
	check(d.bcd == 0x0200, "bcdDevice");
	check(d.dclass == 0xff && d.dsub == 0xff && d.dproto == 0xff,
	      "device class ff/ff/ff");
	check(d.nconfigs == 1 && d.confval == 1, "one configuration, value 1");
	check(d.nifs == 1 && d.ifnum[0] == 0, "one interface, number 0");
	check(d.ifclass[0] == 0xff && d.ifsub[0] == 0xff &&
	      d.ifproto[0] == 0xff, "interface class ff/ff/ff");
	check(d.eptype[USBIP_DIR_IN][4] == USBDEVFS_URB_TYPE_BULK,
	      "ep 0x84 is bulk IN");
	check(d.eptype[USBIP_DIR_OUT][2] == USBDEVFS_URB_TYPE_BULK &&
	      d.eptype[USBIP_DIR_OUT][3] == USBDEVFS_URB_TYPE_BULK &&
	      d.eptype[USBIP_DIR_OUT][4] == USBDEVFS_URB_TYPE_BULK,
	      "ep 2/3/4 are bulk OUT");
	check(d.eptype[USBIP_DIR_OUT][4] >= 0 &&
	      d.eptype[USBIP_DIR_IN][2] < 0,
	      "an OUT endpoint does not appear as IN");
	for (i = 5; i < 16; i++)
		check(d.eptype[USBIP_DIR_IN][i] < 0 &&
		      d.eptype[USBIP_DIR_OUT][i] < 0,
		      "no phantom endpoints");

	/* A truncated blob must be refused, not read past. */
	mute = 1;
	check(parse_descriptors(rtl_desc, 20, 0, &d) < 0,
	      "truncated descriptors are refused");
	mute = 0;
}

static void test_urb_build(void)
{
	/* GET_DESCRIPTOR(device), the first thing the guest ever sends. */
	static const unsigned char setup[8] = {
		0x80, 0x06, 0x00, 0x01, 0x00, 0x00, 0x12, 0x00
	};
	struct usbip_header h;
	struct udev d;
	struct req *r;
	int rc;

	memset(&d, 0, sizeof(d));
	parse_descriptors(rtl_desc, sizeof(rtl_desc), 0, &d);

	memset(&h, 0, sizeof(h));
	h.command = USBIP_CMD_SUBMIT;
	h.seqnum = 1;
	h.direction = USBIP_DIR_IN;
	h.ep = 0;
	h.u.cmd_submit.transfer_buffer_length = 18;
	memcpy(h.u.cmd_submit.setup, setup, 8);

	r = req_alloc(0);
	r->seqnum = h.seqnum;
	r->dir_in = 1;
	r->ctrl = 1;
	r->buflen = 8 + 18;
	r->buf = calloc(1, r->buflen);
	rc = urb_from_pdu(r, &h, &d);
	check(rc == 0, "control URB builds");
	check(r->urb.type == USBDEVFS_URB_TYPE_CONTROL, "type is control");
	check(r->urb.endpoint == (0 | USB_DIR_IN), "endpoint 0x80");
	check(r->urb.buffer_length == 26, "buffer is setup + wLength");
	check_mem(r->buf, setup, 8, "setup packet copied verbatim");
	check(r->urb.usercontext == r, "usercontext points back");
	req_free(r);

	/* A bulk IN on the adapter's 0x84. */
	memset(&h, 0, sizeof(h));
	h.command = USBIP_CMD_SUBMIT;
	h.seqnum = 2;
	h.direction = USBIP_DIR_IN;
	h.ep = 4;
	h.u.cmd_submit.transfer_flags = USBIP_URB_SHORT_NOT_OK |
					USBIP_URB_ISO_ASAP;
	h.u.cmd_submit.transfer_buffer_length = 16384;

	r = req_alloc(0);
	r->seqnum = h.seqnum;
	r->dir_in = 1;
	r->buflen = 16384;
	r->buf = calloc(1, r->buflen);
	rc = urb_from_pdu(r, &h, &d);
	check(rc == 0, "bulk IN URB builds");
	check(r->urb.type == USBDEVFS_URB_TYPE_BULK, "type is bulk");
	check(r->urb.endpoint == 0x84, "endpoint 0x84");
	check(r->urb.flags == USBDEVFS_URB_SHORT_NOT_OK,
	      "ISO_ASAP is dropped on a bulk URB");
	req_free(r);

	/* An endpoint the configuration does not have. */
	memset(&h, 0, sizeof(h));
	h.command = USBIP_CMD_SUBMIT;
	h.seqnum = 3;
	h.direction = USBIP_DIR_IN;
	h.ep = 7;
	r = req_alloc(0);
	r->dir_in = 1;
	r->buflen = 0;
	r->buf = calloc(1, 1);
	mute = 1;
	check(urb_from_pdu(r, &h, &d) == -EPIPE,
	      "an absent endpoint answers -EPIPE");
	mute = 0;
	req_free(r);
}

/*
 * The payload rule, over a real socket: a RET_SUBMIT is followed by
 * actual_length bytes for an IN transfer and by nothing at all for an OUT
 * one. Getting this backwards costs one URB before the client's parser is
 * permanently one buffer behind.
 */
static void test_ret_payload(void)
{
	unsigned char buf[128];
	struct usbip_header h;
	struct req *r;
	int sv[2];
	ssize_t n;

	if (socketpair(AF_UNIX, SOCK_STREAM, 0, sv)) {
		check(0, "socketpair for the payload test");
		return;
	}

	/* control IN: 5 bytes of data live behind the setup packet */
	r = req_alloc(0);
	r->seqnum = 42;
	r->dir_in = 1;
	r->ctrl = 1;
	r->buflen = 8 + 5;
	r->buf = calloc(1, r->buflen);
	memcpy(r->buf + 8, "hello", 5);
	r->urb.actual_length = 5;
	check(send_ret_submit(sv[0], r) == 0, "RET_SUBMIT for control IN");
	req_free(r);

	check(read_full(sv[1], &h, sizeof(h)) == 1, "header arrives");
	hdr_ntoh(&h);
	check(h.command == USBIP_RET_SUBMIT && h.seqnum == 42,
	      "header identifies the URB");
	check(h.u.ret_submit.actual_length == 5, "actual_length is 5");
	check(h.devid == 0 && h.direction == 0 && h.ep == 0,
	      "server zeroes devid/direction/ep");
	check(read_full(sv[1], buf, 5) == 1 && !memcmp(buf, "hello", 5),
	      "the data stage follows the header");

	/* bulk OUT: the same actual_length, but nothing on the wire */
	r = req_alloc(0);
	r->seqnum = 43;
	r->dir_in = 0;
	r->buflen = 5;
	r->buf = calloc(1, r->buflen);
	r->urb.actual_length = 5;
	check(send_ret_submit(sv[0], r) == 0, "RET_SUBMIT for bulk OUT");
	req_free(r);

	check(read_full(sv[1], &h, sizeof(h)) == 1, "OUT header arrives");
	n = recv(sv[1], buf, sizeof(buf), MSG_DONTWAIT);
	check(n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK),
	      "an OUT completion carries no payload");

	/*
	 * iso IN: two packets, the padding between them removed, then the
	 * descriptors -- offset and length echoed from the request so the
	 * client can put the padding back.
	 */
	r = req_alloc(2);
	r->seqnum = 44;
	r->dir_in = 1;
	r->np = 2;
	r->buflen = 8;
	r->buf = calloc(1, r->buflen);
	memcpy(r->buf, "ABCDefgh", 8);
	r->iso[0].offset = 0;
	r->iso[0].length = 4;
	r->iso[1].offset = 512;		/* a gap the client wants back */
	r->iso[1].length = 4;
	r->urb.iso_frame_desc[0].length = 4;
	r->urb.iso_frame_desc[0].actual_length = 4;
	r->urb.iso_frame_desc[1].length = 4;
	r->urb.iso_frame_desc[1].actual_length = 2;
	check(send_ret_submit(sv[0], r) == 0, "RET_SUBMIT for iso IN");
	req_free(r);

	check(read_full(sv[1], &h, sizeof(h)) == 1, "iso header arrives");
	hdr_ntoh(&h);
	check(h.u.ret_submit.number_of_packets == 2, "two iso packets");
	check(h.u.ret_submit.actual_length == 6,
	      "iso actual_length is the sum of the packets");
	check(read_full(sv[1], buf, 6) == 1 && !memcmp(buf, "ABCDef", 6),
	      "iso data is packed, padding removed");
	check(read_full(sv[1], buf, 32) == 1, "two iso descriptors follow");
	check(be32at(buf) == 0 && be32at(buf + 4) == 4 &&
	      be32at(buf + 8) == 4, "descriptor 0 echoes offset/length");
	check(be32at(buf + 16) == 512 && be32at(buf + 24) == 2,
	      "descriptor 1 echoes offset 512, actual_length 2");

	close(sv[0]);
	close(sv[1]);
}

static void test_iso_descriptor(void)
{
	struct usbip_iso_packet p;
	unsigned char raw[16];

	p.offset = htonl(0x100);
	p.length = htonl(0x200);
	p.actual_length = htonl(0x180);
	p.status = htonl((uint32_t)-EXDEV);
	memcpy(raw, &p, sizeof(raw));
	check(raw[3] == 0x00 && raw[2] == 0x01, "iso offset first, big endian");
	check(raw[7] == 0x00 && raw[6] == 0x02, "iso length second");
	check(raw[11] == 0x80 && raw[10] == 0x01, "iso actual_length third");
	check(be32at(&raw[12]) == (uint32_t)-EXDEV,
	      "iso status fourth");
}

static void test_op_import(void)
{
	struct usbip_usb_device ud;
	unsigned char raw[8 + sizeof(ud)];
	struct op_common op;
	struct udev d;

	memset(&d, 0, sizeof(d));
	parse_descriptors(rtl_desc, sizeof(rtl_desc), 0, &d);
	d.busnum = 1;
	d.devnum = 5;
	d.speed = 3;
	snprintf(d.busid, sizeof(d.busid), "1-1");
	snprintf(d.path, sizeof(d.path), "/sys/bus/usb/devices/1-1");

	op.version = htons(USBIP_VERSION);
	op.code = htons(OP_REP_IMPORT);
	op.status = htonl(ST_OK);
	fill_usb_device(&ud, &d);
	memcpy(raw, &op, sizeof(op));
	memcpy(raw + sizeof(op), &ud, sizeof(ud));

	check(raw[0] == 0x01 && raw[1] == 0x11, "version 0x0111 on the wire");
	check(raw[2] == 0x00 && raw[3] == 0x03, "OP_REP_IMPORT code");
	check(!memcmp(raw + 8, "/sys/bus/usb/devices/1-1", 24),
	      "path at offset 8");
	check(!memcmp(raw + 8 + 256, "1-1", 4), "busid at offset 0x108");
	check(be32at(&raw[0x128]) == 1, "busnum at 0x128");
	check(be32at(&raw[0x12c]) == 5, "devnum at 0x12c");
	check(be32at(&raw[0x130]) == 3, "speed at 0x130");
	check(raw[0x134] == 0x23 && raw[0x135] == 0x57, "idVendor at 0x134");
	check(raw[0x136] == 0x01 && raw[0x137] == 0x1f, "idProduct at 0x136");
	check(raw[0x13a] == 0xff, "bDeviceClass at 0x13a");
	check(raw[0x13d] == 0x01, "bConfigurationValue at 0x13d");
	check(raw[0x13e] == 0x01, "bNumConfigurations at 0x13e");
	check(raw[0x13f] == 0x01, "bNumInterfaces at 0x13f");
	check(sizeof(raw) == 320, "OP_REP_IMPORT is 320 bytes");
}

static int selftest(void)
{
	test_layout();
	test_cmd_submit_decode();
	test_ret_submit_encode();
	test_unlink();
	test_descriptors();
	test_urb_build();
	test_ret_payload();
	test_iso_descriptor();
	test_op_import();

	printf("umusb selftest: %s\n", failures ? "FAILED" : "all passed");
	return failures ? 1 : 0;
}

/* ------------------------------------------------------------------ */

static int listen_port;

static int tcp_listen(const char *spec)
{
	struct sockaddr_in sa;
	const char *portstr = strrchr(spec, ':');
	char addr[64] = "127.0.0.1";
	int fd, on = 1;

	if (portstr) {
		size_t n = (size_t)(portstr - spec);

		if (n >= sizeof(addr))
			n = sizeof(addr) - 1;
		memcpy(addr, spec, n);
		addr[n] = '\0';
		portstr++;
	} else {
		portstr = spec;
	}

	listen_port = atoi(portstr);
	memset(&sa, 0, sizeof(sa));
	sa.sin_family = AF_INET;
	sa.sin_port = htons((uint16_t)listen_port);
	if (inet_pton(AF_INET, addr, &sa.sin_addr) != 1) {
		msg("--listen: %s is not an IPv4 address", addr);
		return -1;
	}

	fd = socket(AF_INET, SOCK_STREAM, 0);
	if (fd < 0) {
		msg("socket: %s", strerror(errno));
		return -1;
	}
	setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &on, sizeof(on));
	if (bind(fd, (struct sockaddr *)&sa, sizeof(sa)) ||
	    listen(fd, 1)) {
		msg("binding %s:%s: %s", addr, portstr, strerror(errno));
		close(fd);
		return -1;
	}
	msg("listening on %s:%s -- from the guest that is the passt gateway "
	    "address, e.g. 10.0.2.2:%s", addr, portstr, portstr);
	return fd;
}

static void usage(void)
{
	fprintf(stderr,
"usage: umusb --usbsock NAME | --usbfd N [options] [-- ./linux <uml args...>]\n"
"  --usbfd N       an already-open usbfs descriptor (from Java's\n"
"                  UsbDeviceConnection.getFileDescriptor()/detachFd())\n"
"  --listen [A:]P  serve on TCP A:P (default 127.0.0.1) instead of on a\n"
"                  socketpair; the guest reaches it via passt's gateway\n"
"  --busid ID      override the busid reported to the client\n"
"  --probe-only    print what the descriptor points at and exit, claiming\n"
"                  nothing\n"
"  --no-cmdline    do not append umusb.* parameters to the UML command line\n"
"  --verbose       log every URB\n"
"  --selftest      run the protocol self-test and exit\n");
}

/*
 * Receive the usbfs descriptor over an abstract-namespace unix socket.
 *
 * Passing it by number does not work: measured on the target device, a
 * descriptor opened by the app and handed to a child on the command line
 * arrives closed -- ART's ProcessImpl is OpenJDK's childproc.c, which walks
 * /proc/self/fd in the child and closes everything above stderr before exec.
 * Clearing FD_CLOEXEC does not help, because nothing here is exec'ing on its
 * own behalf. The app therefore keeps the descriptor and sends it, and
 * SCM_RIGHTS installs a new one in this process's table.
 *
 * Abstract namespace (a leading NUL in sun_path) so there is no filesystem
 * object to be denied by SELinux -- an app's own directory is not somewhere it
 * may create a socket that another of its processes can reach by path.
 */
static int recv_usbfd(const char *name)
{
	struct sockaddr_un sa;
	struct msghdr msg = { 0 };
	struct cmsghdr *cm;
	struct iovec iov;
	char cbuf[CMSG_SPACE(sizeof(int))];
	char byte = 0;
	size_t len;
	int s, fd;

	len = strlen(name);
	if (len + 1 > sizeof(sa.sun_path)) {
		fprintf(stderr, "umusb: socket name too long: %s\n", name);
		return -1;
	}

	s = socket(AF_UNIX, SOCK_STREAM, 0);
	if (s < 0) {
		perror("umusb: socket");
		return -1;
	}

	memset(&sa, 0, sizeof(sa));
	sa.sun_family = AF_UNIX;
	sa.sun_path[0] = '\0';
	memcpy(sa.sun_path + 1, name, len);

	if (connect(s, (struct sockaddr *)&sa,
		    offsetof(struct sockaddr_un, sun_path) + 1 + len) < 0) {
		fprintf(stderr, "umusb: connect to @%s: %s\n", name,
			strerror(errno));
		close(s);
		return -1;
	}

	iov.iov_base = &byte;
	iov.iov_len = 1;
	msg.msg_iov = &iov;
	msg.msg_iovlen = 1;
	msg.msg_control = cbuf;
	msg.msg_controllen = sizeof(cbuf);

	if (recvmsg(s, &msg, 0) < 0) {
		fprintf(stderr, "umusb: recvmsg on @%s: %s\n", name,
			strerror(errno));
		close(s);
		return -1;
	}

	cm = CMSG_FIRSTHDR(&msg);
	if (!cm || cm->cmsg_level != SOL_SOCKET ||
	    cm->cmsg_type != SCM_RIGHTS ||
	    cm->cmsg_len != CMSG_LEN(sizeof(int))) {
		fprintf(stderr, "umusb: @%s sent no descriptor\n", name);
		close(s);
		return -1;
	}

	memcpy(&fd, CMSG_DATA(cm), sizeof(fd));
	close(s);
	return fd;
}

int main(int argc, char **argv)
{
	const char *listen_spec = NULL;
	const char *busid = NULL;
	int no_cmdline = 0, do_selftest = 0, probe_only = 0;
	int sp[2] = { -1, -1 }, lfd = -1, sock = -1;
	int i, umlarg = -1, status = 0, rc = 0;
	char args[4][64];
	char **child;

	for (i = 1; i < argc; i++) {
		if (!strcmp(argv[i], "--verbose"))
			verbose = 1;
		else if (!strcmp(argv[i], "--selftest"))
			do_selftest = 1;
		else if (!strcmp(argv[i], "--no-cmdline"))
			no_cmdline = 1;
		else if (!strcmp(argv[i], "--probe-only"))
			probe_only = 1;
		else if (!strcmp(argv[i], "--usbsock") && i + 1 < argc)
			usbsock = argv[++i];
		else if (!strcmp(argv[i], "--usbfd") && i + 1 < argc)
			usbfd = atoi(argv[++i]);
		else if (!strcmp(argv[i], "--listen") && i + 1 < argc)
			listen_spec = argv[++i];
		else if (!strcmp(argv[i], "--busid") && i + 1 < argc)
			busid = argv[++i];
		else if (!strcmp(argv[i], "--")) {
			umlarg = i + 1;
			break;
		} else {
			msg("unknown option %s", argv[i]);
			usage();
			return 2;
		}
	}

	if (do_selftest)
		return selftest();

	if (usbfd < 0 && usbsock)
		usbfd = recv_usbfd(usbsock);

	if (usbfd < 0) {
		msg("no usable descriptor: give --usbsock NAME (preferred) or "
		    "--usbfd N");
		usage();
		return 2;
	}
	if (umlarg >= 0 && umlarg >= argc) {
		msg("nothing to exec after --");
		return 2;
	}

	/*
	 * usbfs only reports completions through POLLOUT, and only for a
	 * descriptor opened for writing. A read-only fd would leave this
	 * process polling forever on URBs that have long since completed, so
	 * say so now rather than hang later.
	 */
	i = fcntl(usbfd, F_GETFL);
	if (i < 0) {
		msg("fd %d: %s", usbfd, strerror(errno));
		return 1;
	}
	if ((i & O_ACCMODE) != O_RDWR) {
		msg("fd %d is not open O_RDWR (flags %#x); usbfs reports "
		    "completions only on a writable descriptor", usbfd, i);
		return 1;
	}
	/* The kernel has no use for the device fd; do not leak it into UML. */
	fcntl(usbfd, F_SETFD, FD_CLOEXEC);

	if (probe_device(usbfd, &dev, busid))
		return 1;

	msg("fd %d: %04x:%04x, bus %u dev %u, %s speed, busid %s, config %u, "
	    "%u interface(s)", usbfd, dev.vid, dev.pid, dev.busnum, dev.devnum,
	    speed_str(dev.speed), dev.busid, dev.confval, dev.nifs);
	if (verbose || probe_only)
		dump_device(&dev);
	/*
	 * vhci_hcd's valid_args() rejects an attach with speed 0, so a device
	 * whose speed could not be read is worth saying out loud here rather
	 * than as a bare -EINVAL from a sysfs write in the guest.
	 */
	if (!dev.speed)
		msg("WARNING: speed came back unknown; the guest's attach "
		    "will refuse speed 0");

	/*
	 * --probe-only stops here, before anything is claimed and before any
	 * driver is disconnected: it answers "is this descriptor the device I
	 * think it is" without disturbing whatever owns it.
	 */
	if (probe_only)
		return 0;

	if (claim_interfaces(usbfd, &dev))
		return 1;

	signal(SIGPIPE, SIG_IGN);	/* a dead client is EPIPE, not death */
	signal(SIGINT, onsig);
	signal(SIGTERM, onsig);

	if (listen_spec) {
		lfd = tcp_listen(listen_spec);
		if (lfd < 0)
			return 1;
	} else {
		/*
		 * SOCK_STREAM because vhci_hcd's attach_store() refuses
		 * anything else outright ("Expecting SOCK_STREAM").
		 */
		if (socketpair(AF_UNIX, SOCK_STREAM, 0, sp)) {
			msg("socketpair: %s", strerror(errno));
			return 1;
		}
		for (i = 0; i < 2; i++) {
			int sz = 1 << 20;

			setsockopt(sp[i], SOL_SOCKET, SO_RCVBUF, &sz,
				   sizeof(sz));
			setsockopt(sp[i], SOL_SOCKET, SO_SNDBUF, &sz,
				   sizeof(sz));
		}
	}

	/*
	 * Everything the guest needs to attach, on stderr and -- unless told
	 * not to -- on its own command line.
	 */
	if (listen_spec)
		snprintf(args[0], sizeof(args[0]), "umusb.port=%d",
			 listen_port);
	else
		snprintf(args[0], sizeof(args[0]), "umusb.fd=%d", sp[0]);
	snprintf(args[1], sizeof(args[1]), "umusb.devid=%u",
		 (dev.busnum << 16) | dev.devnum);
	snprintf(args[2], sizeof(args[2]), "umusb.speed=%u", dev.speed);
	snprintf(args[3], sizeof(args[3]), "umusb.busid=%s", dev.busid);

	msg("attach with: echo \"0 <sockfd> %u %u\" > "
	    "/sys/devices/platform/vhci_hcd.0/attach",
	    (dev.busnum << 16) | dev.devnum, dev.speed);
	if (listen_spec)
		msg("<sockfd> is the guest's own socket connected to the "
		    "listener above");
	else
		msg("<sockfd> is host fd %d, inherited by the kernel process "
		    "-- a UML guest cannot look up a host descriptor, so "
		    "something in the guest has to own this socket", sp[0]);

	if (umlarg >= 0) {
		uml_pid = fork();
		if (uml_pid < 0) {
			msg("fork: %s", strerror(errno));
			return 1;
		}
		if (uml_pid == 0) {
			int n = 0;

			if (lfd >= 0)
				close(lfd);
			if (sp[1] >= 0)
				close(sp[1]);
			/* The descriptor has to survive exec to be of use. */
			if (sp[0] >= 0 && fcntl(sp[0], F_SETFD, 0) < 0) {
				msg("fcntl: %s", strerror(errno));
				_exit(1);
			}

			child = calloc((size_t)(argc - umlarg) + 5,
				       sizeof(char *));
			if (!child)
				_exit(1);
			for (i = umlarg; i < argc; i++)
				child[i - umlarg] = argv[i];
			n = argc - umlarg;
			if (!no_cmdline) {
				child[n++] = args[0];
				child[n++] = args[1];
				child[n++] = args[2];
				child[n++] = args[3];
			}
			child[n] = NULL;

			execvp(child[0], child);
			msg("exec %s: %s", child[0], strerror(errno));
			_exit(127);
		}
		if (sp[0] >= 0) {
			close(sp[0]);
			sp[0] = -1;
		}
	}

	/*
	 * Serve. In listen mode a client may come and go -- a detach followed
	 * by a re-attach is an ordinary thing to do -- so go back to accept()
	 * afterwards; with a socketpair there is only ever the one peer.
	 */
	for (;;) {
		if (lfd >= 0) {
			struct pollfd p = { .fd = lfd, .events = POLLIN };
			int on = 1;

			/*
			 * Wake up now and then to notice a kernel that has
			 * exited: in listen mode nothing else would, and
			 * blocking in accept() for a guest that is gone is
			 * the kind of hang that looks like a protocol bug.
			 */
			if (poll(&p, 1, 500) == 0) {
				if (uml_pid > 0 &&
				    waitpid(uml_pid, &status, WNOHANG) > 0) {
					uml_pid = -1;
					goto done;
				}
				continue;
			}

			sock = accept(lfd, NULL, NULL);
			if (sock < 0) {
				if (errno == EINTR || errno == ECONNABORTED)
					continue;
				msg("accept: %s", strerror(errno));
				rc = 1;
				break;
			}
			/*
			 * Without TCP_NODELAY the small RET_SUBMIT that ends
			 * a control transfer waits for the next segment.
			 */
			setsockopt(sock, IPPROTO_TCP, TCP_NODELAY, &on,
				   sizeof(on));
		} else {
			sock = sp[1];
		}

		if (serve(sock) < 0)
			rc = 1;
		discard_all();
		close(sock);
		sock = -1;

		if (lfd < 0 || rc)
			break;
		msg("waiting for the next client");
	}

done:
	release_interfaces(usbfd, &dev);
	if (lfd >= 0)
		close(lfd);

	/*
	 * Wait for the kernel rather than signalling it. A guest that wrote to
	 * the vhci detach file, or one whose adapter was unplugged, is still a
	 * running guest with a shell on it; killing it because the USB session
	 * ended would be the wrong trade in every case. SIGINT and SIGTERM do
	 * get forwarded -- that is what onsig() is for.
	 *
	 * Its exit status wins over ours when it is nonzero, because that is
	 * what the caller is really waiting on; ours survives when the kernel
	 * left cleanly, so a device error is not reported as success. This is
	 * the failure mode umnet used to have: every possible failure reached
	 * the caller as "exited 0".
	 */
	if (uml_pid > 0) {
		if (waitpid(uml_pid, &status, 0) < 0) {
			msg("waitpid: %s", strerror(errno));
			rc = 1;
		} else if (WIFSIGNALED(status)) {
			msg("kernel killed by signal %d (%s)", WTERMSIG(status),
			    strsignal(WTERMSIG(status)));
			rc = 128 + WTERMSIG(status);
		} else if (WIFEXITED(status) && WEXITSTATUS(status)) {
			msg("kernel exited %d", WEXITSTATUS(status));
			rc = WEXITSTATUS(status);
		}
	} else if (uml_pid < 0) {
		if (WIFSIGNALED(status)) {
			msg("kernel killed by signal %d (%s)", WTERMSIG(status),
			    strsignal(WTERMSIG(status)));
			rc = 128 + WTERMSIG(status);
		} else if (WIFEXITED(status) && WEXITSTATUS(status)) {
			msg("kernel exited %d", WEXITSTATUS(status));
			rc = WEXITSTATUS(status);
		}
	}

	return rc;
}
