// SPDX-License-Identifier: GPL-2.0
/*
 * Host-capability probe for the Android phase.
 *
 * hostcaps.c already covers what the *port* needs from a Linux host: ptrace
 * regsets, personality, mmap_min_addr, fixed mappings, signal frames. This
 * probe covers what is different about a *phone*, which is not the kernel API
 * but the policy wrapped around it. Every check here is something that exists
 * and works on an ordinary Linux box and can be denied on Android.
 *
 * Two of them decide whether UML can boot at all:
 *
 *   1. UML launches its stub process by writing an embedded ELF into a
 *      memfd and calling execveat(fd, "", AT_EMPTY_PATH). Under SELinux a
 *      memfd is labelled with the creating domain's tmpfs type, and executing
 *      it needs an explicit allow rule that the `shell` domain may not have.
 *      The failure would be an EACCES from execveat, not from memfd_create --
 *      and os-Linux only falls back to a temp file when memfd_create itself
 *      fails, so a denial here is fatal rather than degraded.
 *
 *   2. UML backs guest physical memory with a file it mmaps MAP_SHARED. It
 *      wants that file on tmpfs and searches TMPDIR, /dev/shm, /tmp. On
 *      Android none of the usual candidates is both present and writable by
 *      uid shell, so the tempdir has to be named explicitly and it will be on
 *      flash, not tmpfs.
 *
 * Output is one "key = value" per line so it can be diffed between devices.
 *
 * Build:
 *   clang --target=aarch64-linux-gnu -static -O1 -Wall -o androidhost androidhost.c
 */
#define _GNU_SOURCE
#include <stdio.h>
#include <stdarg.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>
#include <fcntl.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <sys/statfs.h>
#include <sys/syscall.h>
#include <sys/wait.h>
#include <sys/resource.h>

#ifndef TMPFS_MAGIC
#define TMPFS_MAGIC 0x01021994
#endif
#ifndef MFD_CLOEXEC
#define MFD_CLOEXEC 0x0001U
#endif
#ifndef AT_EMPTY_PATH
#define AT_EMPTY_PATH 0x1000
#endif

static void kv(const char *k, const char *fmt, ...)
{
	va_list ap;

	printf("%-34s = ", k);
	va_start(ap, fmt);
	vprintf(fmt, ap);
	va_end(ap);
	putchar('\n');
	fflush(stdout);
}

/* A payload that certainly exists on any Android and certainly runs. */
static const char *payload(void)
{
	static const char * const cand[] = {
		"/system/bin/toybox", "/system/bin/toolbox", "/system/bin/sh", NULL
	};
	int i;

	for (i = 0; cand[i]; i++)
		if (access(cand[i], R_OK | X_OK) == 0)
			return cand[i];
	return NULL;
}

static int copy_into(int dst, const char *src)
{
	char buf[65536];
	ssize_t n;
	int fd = open(src, O_RDONLY | O_CLOEXEC);

	if (fd < 0)
		return -1;
	while ((n = read(fd, buf, sizeof(buf))) > 0) {
		ssize_t off = 0;

		while (off < n) {
			ssize_t w = write(dst, buf + off, n - off);

			if (w <= 0) { close(fd); return -1; }
			off += w;
		}
	}
	close(fd);
	return n < 0 ? -1 : 0;
}

/*
 * Exec the fd and report what happened. The child is the only thing that can
 * fail here, so the errno has to travel back over a pipe -- a nonzero exit
 * status alone would not distinguish "denied" from "ran and exited nonzero".
 */
static void try_exec_fd(const char *what, int fd)
{
	char *const argv[] = { (char *)"true", NULL };
	char *const envp[] = { NULL };
	int pfd[2], status, err = 0;
	pid_t pid;

	if (pipe(pfd)) { kv(what, "pipe failed"); return; }

	pid = fork();
	if (pid == 0) {
		close(pfd[0]);
		syscall(__NR_execveat, fd, "", argv, envp, AT_EMPTY_PATH);
		err = errno;
		write(pfd[1], &err, sizeof(err));
		_exit(127);
	}
	close(pfd[1]);
	if (read(pfd[0], &err, sizeof(err)) != sizeof(err))
		err = 0;			/* nothing written: exec succeeded */
	close(pfd[0]);
	waitpid(pid, &status, 0);

	if (err)
		kv(what, "DENIED errno=%d (%s)", err, strerror(err));
	else
		kv(what, "OK (child exit %d)", WIFEXITED(status) ? WEXITSTATUS(status) : -1);
}

static void check_dir(const char *dir)
{
	char key[128], path[256];
	struct statfs sf;
	int fd;

	if (!dir)
		return;

	snprintf(key, sizeof(key), "dir%s", dir);
	if (statfs(dir, &sf)) {
		kv(key, "absent (errno=%d)", errno);
		return;
	}

	snprintf(path, sizeof(path), "%s/.umarm-probe-XXXXXX", dir);
	fd = mkstemp(path);
	if (fd < 0) {
		kv(key, "fstype=0x%lx not-writable errno=%d",
		   (unsigned long)sf.f_type, errno);
		return;
	}
	unlink(path);

	/* The real question is not "can I create a file" but "can I use it as
	 * guest RAM": ftruncate to a size, map it shared, and touch it. */
	{
		const size_t sz = 64u << 20;
		void *p;
		int ok = 0;

		if (ftruncate(fd, sz) == 0) {
			p = mmap(NULL, sz, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
			if (p != MAP_FAILED) {
				((volatile char *)p)[0] = 0x5a;
				((volatile char *)p)[sz - 1] = 0xa5;
				ok = ((volatile char *)p)[0] == 0x5a &&
				     ((volatile char *)p)[sz - 1] == (char)0xa5;
				munmap(p, sz);
			}
		}
		kv(key, "fstype=0x%lx%s writable mmap_shared_64M=%s",
		   (unsigned long)sf.f_type,
		   sf.f_type == TMPFS_MAGIC ? " (tmpfs)" : "",
		   ok ? "yes" : "no");
	}
	close(fd);
}

int main(void)
{
	const char *pl = payload();
	struct rlimit rl;
	int fd;

	setvbuf(stdout, NULL, _IONBF, 0);
	printf("ANDROIDHOST_START\n");

	kv("uid", "%d", (int)getuid());
	kv("page_size", "%ld", sysconf(_SC_PAGESIZE));
	kv("nproc", "%ld", sysconf(_SC_NPROCESSORS_ONLN));
	kv("payload", "%s", pl ? pl : "NONE FOUND");

	/* --- the one that decides whether the stub can start --- */
	fd = syscall(__NR_memfd_create, "uml-userspace", MFD_CLOEXEC);
	if (fd < 0) {
		kv("memfd_create", "FAILED errno=%d (%s) -- os-Linux falls back to a temp file",
		   errno, strerror(errno));
	} else {
		kv("memfd_create", "OK fd=%d", fd);
		if (pl && copy_into(fd, pl) == 0) {
			fchmod(fd, 00500);
			try_exec_fd("execveat(memfd)", fd);
		} else {
			kv("execveat(memfd)", "skipped: no payload");
		}
		close(fd);
	}

	/* --- the fallback path, which must work if the above does not --- */
	if (pl) {
		char path[256] = "/data/local/tmp/.umarm-stub-XXXXXX";
		int tfd = mkstemp(path);

		if (tfd < 0) {
			kv("execveat(tmpfile)", "mkstemp failed errno=%d", errno);
		} else if (copy_into(tfd, pl) == 0) {
			int rfd;

			fchmod(tfd, 00500);
			close(tfd);
			rfd = open(path, O_RDONLY | O_CLOEXEC);
			if (rfd >= 0) {
				try_exec_fd("execveat(tmpfile)", rfd);
				close(rfd);
			} else {
				kv("execveat(tmpfile)", "reopen failed errno=%d", errno);
			}
		}
		unlink(path);
	}

	/* --- where guest RAM can live --- */
	check_dir(getenv("TMPDIR"));
	check_dir("/dev/shm");
	check_dir("/tmp");
	check_dir("/data/local/tmp");
	check_dir("/mnt");
	check_dir("/sdcard");

	if (!getrlimit(RLIMIT_NOFILE, &rl))
		kv("rlimit.nofile", "%lu/%lu", (unsigned long)rl.rlim_cur, (unsigned long)rl.rlim_max);
	if (!getrlimit(RLIMIT_AS, &rl))
		kv("rlimit.as", "%ld", (long)rl.rlim_cur);
	if (!getrlimit(RLIMIT_STACK, &rl))
		kv("rlimit.stack", "%lu", (unsigned long)rl.rlim_cur);

	kv("proc.self.exe", "%s", access("/proc/self/exe", R_OK) == 0 ? "readable" : "denied");
	kv("proc.self.maps", "%s", access("/proc/self/maps", R_OK) == 0 ? "readable" : "denied");

	printf("ANDROIDHOST_DONE\n");
	return 0;
}
