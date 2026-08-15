// SPDX-License-Identifier: GPL-2.0
/*
 * Guest-side probe: does /dev/ubda work?
 *
 * On the phone, booting with root=/dev/ubda produces no ubd messages, no VFS
 * panic and no output at all -- the kernel simply stops after console init and
 * the process exits 0. That is unreadable from outside: it cannot be told apart
 * from a hang, a crash, or a mount that failed quietly.
 *
 * Running as init from an initramfs puts a working console and a working guest
 * underneath the question, so whatever /dev/ubda does can be printed rather
 * than inferred. It reports the device's size, the result of a read, and errno
 * on failure, which distinguishes "the block device is absent", "it is present
 * and reads fail", and "reads work and the earlier failure was in mounting".
 */
#define _GNU_SOURCE
#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>
#include <fcntl.h>
#include <sys/mount.h>
#include <sys/stat.h>
#include <sys/ioctl.h>
#include <linux/fs.h>
#include <sys/sysmacros.h>

static void hexdump(const unsigned char *p, int n)
{
	int i;

	for (i = 0; i < n; i++)
		printf("%02x%s", p[i], (i % 16) == 15 ? "\n" : " ");
}

int main(void)
{
	unsigned char buf[512];
	unsigned long long sz = 0;
	struct stat st;
	ssize_t n;
	int fd;

	/*
	 * init inherits no file descriptors: printf before this goes to a
	 * closed fd 1 and is simply lost, which looks exactly like the guest
	 * never having run. Mount devtmpfs and claim the console first, then
	 * say anything.
	 */
	int mount_err;

	mkdir("/dev", 0755);
	mount_err = (mount("devtmpfs", "/dev", "devtmpfs", 0, NULL) &&
		     errno != EBUSY) ? errno : 0;

	fd = open("/dev/console", O_RDWR);
	if (fd >= 0) {
		dup2(fd, 0);
		dup2(fd, 1);
		dup2(fd, 2);
		if (fd > 2)
			close(fd);
	}

	setvbuf(stdout, NULL, _IONBF, 0);
	printf("UBDTEST_START\n");
	printf("mount devtmpfs: %s\n",
	       mount_err ? strerror(mount_err) : "ok");

	if (stat("/dev/ubda", &st))
		printf("stat /dev/ubda: errno=%d (%s)  <- device node absent\n",
		       errno, strerror(errno));
	else
		printf("stat /dev/ubda: ok, rdev=%u:%u\n",
		       major(st.st_rdev), minor(st.st_rdev));

	fd = open("/dev/ubda", O_RDONLY);
	if (fd < 0) {
		printf("open /dev/ubda: errno=%d (%s)\n", errno, strerror(errno));
		goto done;
	}
	printf("open /dev/ubda: ok\n");

	if (ioctl(fd, BLKGETSIZE64, &sz))
		printf("BLKGETSIZE64: errno=%d\n", errno);
	else
		printf("BLKGETSIZE64: %llu bytes (%llu MiB)\n", sz, sz >> 20);

	n = read(fd, buf, sizeof(buf));
	if (n < 0) {
		printf("read: errno=%d (%s)\n", errno, strerror(errno));
	} else {
		printf("read: %zd bytes, first 32:\n", n);
		hexdump(buf, 32);
	}

	/* An ext4 superblock starts at offset 1024; magic 0xEF53 at +56. */
	if (pread(fd, buf, sizeof(buf), 1024) == (ssize_t)sizeof(buf)) {
		unsigned magic = buf[56] | (buf[57] << 8);

		printf("ext4 magic at 1024+56: 0x%04x %s\n", magic,
		       magic == 0xEF53 ? "(correct)" : "(WRONG)");
	} else {
		printf("pread at 1024: errno=%d\n", errno);
	}

	close(fd);
done:
	printf("UBDTEST_DONE\n");
	printf("UMARM_BOOT_OK\n");
	return 0;
}
