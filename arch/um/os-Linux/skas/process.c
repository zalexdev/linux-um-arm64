// SPDX-License-Identifier: GPL-2.0
/*
 * Copyright (C) 2021 Benjamin Berg <benjamin@sipsolutions.net>
 * Copyright (C) 2015 Thomas Meyer (thomas@m3y3r.de)
 * Copyright (C) 2002- 2007 Jeff Dike (jdike@{addtoit,linux.intel}.com)
 */

#include <stdlib.h>
#include <stdbool.h>
#include <unistd.h>
#include <sched.h>
#include <errno.h>
#include <string.h>
#include <fcntl.h>
#include <mem_user.h>

#include <sys/mman.h>
#include <sys/wait.h>
#include <sys/stat.h>
#include <sys/socket.h>
#include <asm/unistd.h>
#include <as-layout.h>
#include <init.h>
#include <kern_util.h>
#include <mem.h>
#include <os.h>
#include <ptrace_user.h>
#include <registers.h>
#include <skas.h>
#include <sysdep/stub.h>
#include <stub-futex.h>
#include <sysdep/mcontext.h>
#include <linux/futex.h>
#include <linux/threads.h>
#include <timetravel.h>
/*
 * __READ_ONCE, without <asm-generic/rwonce.h>.
 *
 * That header needs the kernel's compiler attributes, and a USER_CFLAGS object
 * cannot rely on reaching them: arch/um/Makefile adds the kernel's include
 * directory with -idirafter, i.e. *after* the system directories, so a sysroot
 * that ships its own linux/compiler_types.h wins. The Android NDK's sysroot
 * does exactly that, and rwonce.h then fails on __no_sanitize_or_inline.
 *
 * Force-including the kernel's compiler_types.h instead is not an option: it
 * defines __private, which bionic uses as a struct member name in
 * <bits/pthread_types.h>, so every pthread type stops compiling.
 *
 * This file uses one macro from that header and nothing else, and this is the
 * kernel's own uninstrumented definition of it.
 */
#ifndef __READ_ONCE
#define __READ_ONCE(x)	(*(const volatile __typeof__(x) *)&(x))
#endif
#include "../internal.h"

int is_skas_winch(int pid, int fd, void *data)
{
	return pid == getpgrp();
}

static int ptrace_dump_regs(int pid)
{
	unsigned long regs[MAX_REG_NR];
	int i;

	if (get_host_regs(pid, regs) < 0)
		return -errno;

	printk(UM_KERN_ERR "Stub registers -\n");
	for (i = 0; i < ARRAY_SIZE(regs); i++) {
		const char *regname = ptrace_reg_name(i);

		printk(UM_KERN_ERR "\t%s\t(%2d): %lx\n", regname, i, regs[i]);
	}

	return 0;
}

/*
 * Signals that are OK to receive in the stub - we'll just continue it.
 * SIGWINCH will happen when UML is inside a detached screen.
 */
#define STUB_SIG_MASK ((1 << SIGALRM) | (1 << SIGWINCH))

/* Signals that the stub will finish with - anything else is an error */
#define STUB_DONE_MASK (1 << SIGTRAP)

void wait_stub_done(int pid)
{
	int n, status, err;

	while (1) {
		CATCH_EINTR(n = waitpid(pid, &status, WUNTRACED | __WALL));
		if ((n < 0) || !WIFSTOPPED(status))
			goto bad_wait;

		if (((1 << WSTOPSIG(status)) & STUB_SIG_MASK) == 0)
			break;

		err = ptrace(PTRACE_CONT, pid, 0, 0);
		if (err) {
			printk(UM_KERN_ERR "%s : continue failed, errno = %d\n",
			       __func__, errno);
			fatal_sigsegv();
		}
	}

	if (((1 << WSTOPSIG(status)) & STUB_DONE_MASK) != 0)
		return;

bad_wait:
	err = ptrace_dump_regs(pid);
	if (err)
		printk(UM_KERN_ERR "Failed to get registers from stub, errno = %d\n",
		       -err);
	printk(UM_KERN_ERR "%s : failed to wait for SIGTRAP, pid = %d, n = %d, errno = %d, status = 0x%x\n",
	       __func__, pid, n, errno, status);
	fatal_sigsegv();
}

/*
 * How long each side may busy-poll the handoff word before parking, in
 * microseconds. The measured 22 us seccomp syscall round trip is mostly
 * handoff overhead around a few microseconds of actual work, so 10 us covers
 * the reply for the syscalls that matter while capping what a miss can burn
 * at roughly the cost of the futex sleep it tried to avoid. The hit-streak
 * accounting (see stub-futex.h) keeps an idle or compute-bound guest from
 * paying even that on every wait.
 */
#define SECCOMP_SPIN_DEFAULT_US 10

static unsigned long seccomp_spin_us = SECCOMP_SPIN_DEFAULT_US;

static int __init uml_seccomp_spin_setup(char *line, int *add)
{
	*add = 0;
	seccomp_spin_us = strtoul(line, NULL, 0);

	/*
	 * Anything beyond a scheduling quantum defeats the purpose and risks
	 * the budget arithmetic overflowing spin_ticks; clamp, don't reject.
	 */
	if (seccomp_spin_us > 100000)
		seccomp_spin_us = 100000;

	return 0;
}

__uml_setup("seccomp_spin=", uml_seccomp_spin_setup,
"seccomp_spin=<microseconds>\n"
"    In SECCOMP mode, busy-poll the stub<->kernel handoff word this long\n"
"    before sleeping in FUTEX_WAIT, so a fast peer is observed without the\n"
"    futex syscalls and the context switch they cost. Bounded by wall time\n"
"    (the constant-rate architected counter), not iterations, so it means\n"
"    the same thing at every CPU frequency. An adaptive hit-streak stops\n"
"    the polling when the peer keeps being slow. 0 disables polling.\n"
"    Default: 10 (SECCOMP_SPIN_DEFAULT_US).\n\n");

void wait_stub_done_seccomp(struct mm_id *mm_idp, int running, int wait_sigsys)
{
	struct stub_data *data = (void *)mm_idp->stack;
	unsigned long budget;
	unsigned int futex_val;
	int ret;

	do {
		const char byte = 0;
		struct iovec iov = {
			.iov_base = (void *)&byte,
			.iov_len = sizeof(byte),
		};
		union {
			char data[CMSG_SPACE(sizeof(mm_idp->syscall_fd_map))];
			struct cmsghdr align;
		} ctrl;
		struct msghdr msgh = {
			.msg_iov = &iov,
			.msg_iovlen = 1,
		};

		if (!running) {
			if (mm_idp->syscall_fd_num) {
				unsigned int fds_size =
					sizeof(int) * mm_idp->syscall_fd_num;
				struct cmsghdr *cmsg;

				msgh.msg_control = ctrl.data;
				msgh.msg_controllen = CMSG_SPACE(fds_size);
				cmsg = CMSG_FIRSTHDR(&msgh);
				cmsg->cmsg_level = SOL_SOCKET;
				cmsg->cmsg_type = SCM_RIGHTS;
				cmsg->cmsg_len = CMSG_LEN(fds_size);
				memcpy(CMSG_DATA(cmsg), mm_idp->syscall_fd_map,
				       fds_size);

				CATCH_EINTR(syscall(__NR_sendmsg, mm_idp->sock,
						&msgh, 0));
			}

			data->signal = 0;
			/*
			 * The exchange's release ordering publishes signal and
			 * syscall_data before the stub can observe the flip;
			 * the returned old value elides the FUTEX_WAKE when
			 * the stub's spin caught the reply and never parked.
			 */
			if (stub_futex_hand_over(&data->futex, FUTEX_IN_CHILD))
				CATCH_EINTR(syscall(__NR_futex, &data->futex,
						    FUTEX_WAKE, 1, NULL, NULL, 0));
		}

		/*
		 * A stub that answers within the budget is observed by the
		 * spin without ever entering FUTEX_WAIT. A dead child cannot
		 * flip the word, so the spin can only cost its bounded budget
		 * before the PID checks below run as they always did.
		 */
		budget = stub_futex_spin_budget(&data->kern_spin,
						data->spin_ticks);
		futex_val = stub_futex_spin(&data->futex, FUTEX_IN_CHILD,
					    budget);
		if (budget)
			stub_futex_spin_result(&data->kern_spin,
					       STUB_FUTEX_OWNER(futex_val) == FUTEX_IN_KERN);

		/*
		 * Going to sleep: advertise it via the waiter bit so the
		 * stub's handoff knows to send a wake. The fetch_or returns
		 * the old value, so a flip that happened since the spin gave
		 * up is seen here and skips the wait entirely.
		 */
		if (STUB_FUTEX_OWNER(futex_val) == FUTEX_IN_CHILD)
			futex_val = stub_futex_fetch_or(&data->futex,
							STUB_FUTEX_WAITER) |
				STUB_FUTEX_WAITER;

		while (STUB_FUTEX_OWNER(futex_val) == FUTEX_IN_CHILD) {
			/*
			 * We need to check whether the child is still alive
			 * before and after the FUTEX_WAIT call. Before, in
			 * case it just died but we still updated data->futex
			 * to FUTEX_IN_CHILD. And after, in case it died while
			 * we were waiting (and SIGCHLD woke us up, see the
			 * IRQ handler in mmu.c).
			 *
			 * Either way, if PID is negative, then we have no
			 * choice but to kill the task.
			 */
			if (__READ_ONCE(mm_idp->pid) < 0)
				goto out_kill;

			/*
			 * The expected value is the freshly observed one, not
			 * the constant: if the stub (or the death path in
			 * mm_sigchld_irq) flipped the word between the read
			 * and this call, FUTEX_WAIT returns EAGAIN instead of
			 * sleeping through the wake, and the acquire re-read
			 * below picks the flip up.
			 */
			ret = syscall(__NR_futex, &data->futex,
				      FUTEX_WAIT, futex_val,
				      NULL, NULL, 0);
			if (ret < 0 && errno != EINTR && errno != EAGAIN) {
				printk(UM_KERN_ERR "%s : FUTEX_WAIT failed, errno = %d\n",
				       __func__, errno);
				goto out_kill;
			}

			futex_val = stub_futex_load_acquire(&data->futex);
		}

		if (__READ_ONCE(mm_idp->pid) < 0)
			goto out_kill;

		running = 0;

		/* We may receive a SIGALRM before SIGSYS, iterate again. */
	} while (wait_sigsys && data->signal == SIGALRM);

	/*
	 * Both halves matter. The subtraction underflows when a signal frame is
	 * larger than sigstack[], which is not hypothetical: an arm64 mcontext_t
	 * is around 4.4 KB against x86-64's few hundred bytes, so with 4 KB
	 * pages it is bigger than the single page sigstack[] is. Without the
	 * first test the bound becomes SIZE_MAX and every offset passes it,
	 * which turns a stub that cannot work on this host into an
	 * out-of-bounds read that looks like a crash somewhere else.
	 */
	if (sizeof(data->sigstack) < sizeof(mcontext_t) ||
	    data->mctx_offset > sizeof(data->sigstack) - sizeof(mcontext_t)) {
		printk(UM_KERN_ERR "%s : invalid mcontext offset", __func__);
		goto out_kill;
	}

	if (wait_sigsys && data->signal != SIGSYS) {
		printk(UM_KERN_ERR "%s : expected SIGSYS but got %d",
		       __func__, data->signal);
		goto out_kill;
	}

	return;

out_kill:
	printk(UM_KERN_ERR "%s : failed to wait for stub, pid = %d, errno = %d\n",
	       __func__, mm_idp->pid, errno);
	/* This is not true inside start_userspace */
	if (current_mm_id() == mm_idp)
		fatal_sigsegv();
}

extern unsigned long current_stub_stack(void);

static void get_skas_faultinfo(int pid, struct faultinfo *fi)
{
	int err;

	err = ptrace(PTRACE_CONT, pid, 0, SIGSEGV);
	if (err) {
		printk(UM_KERN_ERR "Failed to continue stub, pid = %d, "
		       "errno = %d\n", pid, errno);
		fatal_sigsegv();
	}
	wait_stub_done(pid);

	/*
	 * faultinfo is prepared by the stub_segv_handler at start of
	 * the stub stack page. We just have to copy it.
	 */
	memcpy(fi, (void *)current_stub_stack(), sizeof(*fi));
}

static void handle_trap(struct uml_pt_regs *regs)
{
	if ((UPT_IP(regs) >= STUB_START) && (UPT_IP(regs) < STUB_END))
		fatal_sigsegv();

	handle_syscall(regs);
}

extern char __syscall_stub_start[];

static int stub_exe_fd;

struct tramp_data {
	struct stub_data *stub_data;
	/* 0 is inherited, 1 is the kernel side */
	int sockpair[2];
};

#ifndef CLOSE_RANGE_CLOEXEC
#define CLOSE_RANGE_CLOEXEC	(1U << 2)
#endif

static int userspace_tramp(void *data)
{
	struct tramp_data *tramp_data = data;
	char *const argv[] = { "uml-userspace", NULL };
	unsigned long long offset;
	struct stub_init_data init_data = {
		.seccomp = using_seccomp,
		.stub_start = STUB_START,
	};
	int ret;

	if (using_seccomp) {
		init_data.signal_handler = STUB_CODE +
					   (unsigned long) stub_signal_interrupt -
					   (unsigned long) __syscall_stub_start;
		init_data.signal_restorer = STUB_CODE +
					   (unsigned long) stub_signal_restorer -
					   (unsigned long) __syscall_stub_start;
	} else {
		init_data.signal_handler = STUB_CODE +
					   (unsigned long) stub_segv_handler -
					   (unsigned long) __syscall_stub_start;
		init_data.signal_restorer = 0;
	}

	init_data.stub_code_fd = phys_mapping(uml_to_phys(__syscall_stub_start),
					      &offset);
	init_data.stub_code_offset = MMAP_OFFSET(offset);

	init_data.stub_data_fd = phys_mapping(uml_to_phys(tramp_data->stub_data),
					      &offset);
	init_data.stub_data_offset = MMAP_OFFSET(offset);

	/*
	 * Avoid leaking unneeded FDs to the stub by setting CLOEXEC on all FDs
	 * and then unsetting it on all memory related FDs.
	 * This is not strictly necessary from a safety perspective.
	 */
	syscall(__NR_close_range, 0, ~0U, CLOSE_RANGE_CLOEXEC);

	fcntl(init_data.stub_data_fd, F_SETFD, 0);

	/* dup2 signaling FD/socket to STDIN */
	if (dup2(tramp_data->sockpair[0], 0) < 0)
		exit(3);
	close(tramp_data->sockpair[0]);

	/* Write init_data and close write side */
	ret = write(tramp_data->sockpair[1], &init_data, sizeof(init_data));
	close(tramp_data->sockpair[1]);

	if (ret != sizeof(init_data))
		exit(4);

	/* Raw execveat for compatibility with older libc versions */
	syscall(__NR_execveat, stub_exe_fd, (unsigned long)"",
		(unsigned long)argv, NULL, AT_EMPTY_PATH);

	exit(5);
}

extern char stub_exe_start[];
extern char stub_exe_end[];

extern char *tempdir;

#define STUB_EXE_NAME_TEMPLATE "/uml-userspace-XXXXXX"

#ifndef MFD_EXEC
#define MFD_EXEC 0x0010U
#endif

static char *stub_exe_path __initdata;

static int __init uml_stub_exe_setup(char *line, int *add)
{
	*add = 0;
	stub_exe_path = line;
	return 0;
}

__uml_setup("stub_exe=", uml_stub_exe_setup,
"stub_exe=<path>\n"
"    Execute the stub from this file instead of from an anonymous memfd.\n"
"\n"
"    UML normally writes its embedded stub into a memfd and execs that, which\n"
"    needs nothing from the filesystem. Android forbids it: an app process runs\n"
"    in the untrusted_app SELinux domain under a W^X policy, where the only\n"
"    place it may execute from is the native library directory unpacked from\n"
"    its APK. Neither a memfd nor a file the app wrote itself is executable\n"
"    there.\n"
"\n"
"    Point this at a copy of arch/um/kernel/skas/stub_exe shipped as one of\n"
"    those libraries. The file must be exactly that stub: it is exec'd as the\n"
"    userspace half of every guest address space.\n\n"
);

static int __init init_stub_exe_fd(void)
{
	size_t written = 0;
	char *tmpfile = NULL;

	/*
	 * An explicit path bypasses the memfd entirely -- there is nothing to
	 * write, because the file already holds the stub.
	 */
	if (stub_exe_path) {
		stub_exe_fd = open(stub_exe_path, O_RDONLY | O_CLOEXEC);
		if (stub_exe_fd < 0)
			panic("Could not open stub binary '%s': %d",
			      stub_exe_path, -errno);
		return 0;
	}

	stub_exe_fd = memfd_create("uml-userspace",
				   MFD_EXEC | MFD_CLOEXEC | MFD_ALLOW_SEALING);

	/*
	 * MFD_EXEC only exists from 6.3. Older kernels reject unknown
	 * memfd_create flags with EINVAL -- and on those a memfd is executable
	 * anyway, since MFD_EXEC was introduced precisely to make that default
	 * something a process could opt out of. Retry without it.
	 *
	 * Worth the retry rather than accepting the temporary-file fallback:
	 * that path needs a tmpdir that permits execution, and on Android the
	 * obvious candidate is /tmp, which is tmpfs and whose SELinux label the
	 * shell domain may not be allowed to execute from. The memfd works
	 * there today; a file on disk is one policy decision away from not.
	 */
	if (stub_exe_fd < 0 && errno == EINVAL)
		stub_exe_fd = memfd_create("uml-userspace",
					   MFD_CLOEXEC | MFD_ALLOW_SEALING);

	if (stub_exe_fd < 0) {
		printk(UM_KERN_INFO "Could not create executable memfd, using temporary file!");

		tmpfile = malloc(strlen(tempdir) +
				  strlen(STUB_EXE_NAME_TEMPLATE) + 1);
		if (tmpfile == NULL)
			panic("Failed to allocate memory for stub binary name");

		strcpy(tmpfile, tempdir);
		strcat(tmpfile, STUB_EXE_NAME_TEMPLATE);

		stub_exe_fd = mkstemp(tmpfile);
		if (stub_exe_fd < 0)
			panic("Could not create temporary file for stub binary: %d",
			      -errno);
	}

	while (written < stub_exe_end - stub_exe_start) {
		ssize_t res = write(stub_exe_fd, stub_exe_start + written,
				    stub_exe_end - stub_exe_start - written);
		if (res < 0) {
			if (errno == EINTR)
				continue;

			if (tmpfile)
				unlink(tmpfile);
			panic("Failed write stub binary: %d", -errno);
		}

		written += res;
	}

	if (!tmpfile) {
		fcntl(stub_exe_fd, F_ADD_SEALS,
		      F_SEAL_WRITE | F_SEAL_SHRINK | F_SEAL_GROW | F_SEAL_SEAL);
	} else {
		if (fchmod(stub_exe_fd, 00500) < 0) {
			unlink(tmpfile);
			panic("Could not make stub binary executable: %d",
			      -errno);
		}

		close(stub_exe_fd);
		stub_exe_fd = open(tmpfile, O_RDONLY | O_CLOEXEC | O_NOFOLLOW);
		if (stub_exe_fd < 0) {
			unlink(tmpfile);
			panic("Could not reopen stub binary: %d", -errno);
		}

		unlink(tmpfile);
		free(tmpfile);
	}

	return 0;
}
__initcall(init_stub_exe_fd);

int using_seccomp;

/*
 * Assume the host has PTRACE_SYSEMU until check_sysemu() says otherwise. Every
 * host that can run x86 UML has had it since 2.6, so this default is what that
 * architecture sees; arm64 hosts older than 5.3 flip it to 0.
 */
int have_ptrace_sysemu = 1;
int syscall_cancel_nr = -1;

/**
 * start_userspace() - prepare a new userspace process
 * @mm_id: The corresponding struct mm_id
 *
 * Setups a new temporary stack page that is used while userspace_tramp() runs
 * Clones the kernel process into a new userspace process, with FDs only.
 *
 * Return: When positive: the process id of the new userspace process,
 *         when negative: an error number.
 * FIXME: can PIDs become negative?!
 */
int start_userspace(struct mm_id *mm_id)
{
	struct stub_data *proc_data = (void *)mm_id->stack;
	struct tramp_data tramp_data = {
		.stub_data = proc_data,
	};
	void *stack;
	unsigned long sp;
	int status, n, err;

	/* setup a temporary stack page */
	stack = mmap(NULL, UM_KERN_PAGE_SIZE,
		     PROT_READ | PROT_WRITE | PROT_EXEC,
		     MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
	if (stack == MAP_FAILED) {
		err = -errno;
		printk(UM_KERN_ERR "%s : mmap failed, errno = %d\n",
		       __func__, errno);
		return err;
	}

	/* set stack pointer to the end of the stack page, so it can grow downwards */
	sp = (unsigned long)stack + UM_KERN_PAGE_SIZE;

	/* socket pair for init data and SECCOMP FD passing (no CLOEXEC here) */
	if (socketpair(AF_UNIX, SOCK_STREAM, 0, tramp_data.sockpair)) {
		err = -errno;
		printk(UM_KERN_ERR "%s : socketpair failed, errno = %d\n",
		       __func__, errno);
		return err;
	}

	if (using_seccomp) {
		proc_data->futex = FUTEX_IN_CHILD;
		/*
		 * Convert the microsecond budget into counter ticks once per
		 * stub, before it first runs; the stub cannot compute this
		 * itself without re-reading the counter frequency on every
		 * trap. stub_cycles_per_us() is 0 where no constant-rate EL0
		 * counter is available (x86), which disables the spin but
		 * keeps the waiter-bit protocol.
		 */
		proc_data->spin_ticks = seccomp_spin_us * stub_cycles_per_us();
	}

	mm_id->pid = clone(userspace_tramp, (void *) sp,
		    CLONE_VFORK | CLONE_VM | SIGCHLD,
		    (void *)&tramp_data);
	if (mm_id->pid < 0) {
		err = -errno;
		printk(UM_KERN_ERR "%s : clone failed, errno = %d\n",
		       __func__, errno);
		goto out_close;
	}

	if (using_seccomp) {
		wait_stub_done_seccomp(mm_id, 1, 1);
	} else {
		do {
			CATCH_EINTR(n = waitpid(mm_id->pid, &status,
						WUNTRACED | __WALL));
			if (n < 0) {
				err = -errno;
				printk(UM_KERN_ERR "%s : wait failed, errno = %d\n",
				       __func__, errno);
				goto out_kill;
			}
		} while (WIFSTOPPED(status) && (WSTOPSIG(status) == SIGALRM));

		if (!WIFSTOPPED(status) || (WSTOPSIG(status) != SIGSTOP)) {
			err = -EINVAL;
			printk(UM_KERN_ERR "%s : expected SIGSTOP, got status = %d\n",
			       __func__, status);
			goto out_kill;
		}

		if (ptrace(PTRACE_SETOPTIONS, mm_id->pid, NULL,
			   (void *) PTRACE_O_TRACESYSGOOD) < 0) {
			err = -errno;
			printk(UM_KERN_ERR "%s : PTRACE_SETOPTIONS failed, errno = %d\n",
			       __func__, errno);
			goto out_kill;
		}
	}

	if (munmap(stack, UM_KERN_PAGE_SIZE) < 0) {
		err = -errno;
		printk(UM_KERN_ERR "%s : munmap failed, errno = %d\n",
		       __func__, errno);
		goto out_kill;
	}

	close(tramp_data.sockpair[0]);
	if (using_seccomp)
		mm_id->sock = tramp_data.sockpair[1];
	else
		close(tramp_data.sockpair[1]);

	return 0;

out_kill:
	os_kill_ptraced_process(mm_id->pid, 1);
out_close:
	close(tramp_data.sockpair[0]);
	close(tramp_data.sockpair[1]);

	mm_id->pid = -1;

	return err;
}

static int unscheduled_userspace_iterations;
extern unsigned long tt_extra_sched_jiffies;

#ifdef UM_SYSCALL_TRAP_INSN
/*
 * Is the guest about to execute the instruction that enters the kernel?
 *
 * Only consulted while single-stepping a guest on a host without
 * PTRACE_SYSEMU, which is a debugger path, so the extra ptrace call per step
 * costs nothing that matters. A failed read is reported as "not a syscall":
 * the instruction is about to be executed by the guest either way, and if it
 * is unreadable the single-step will fault on it and be reported normally,
 * which is a better outcome than refusing to step.
 */
static int at_syscall_insn(int pid, unsigned long pc)
{
	long word;

	errno = 0;
	word = ptrace(PTRACE_PEEKTEXT, pid, (void *)pc, 0);
	if (word == -1 && errno)
		return 0;

	return (unsigned int)word == UM_SYSCALL_TRAP_INSN;
}
#endif

void userspace(struct uml_pt_regs *regs)
{
	int err, status, op;
	siginfo_t si_local;
	siginfo_t *si;
	int sig;

	/* Handle any immediate reschedules or signals */
	interrupt_end();

	while (1) {
		struct mm_id *mm_id = current_mm_id();

		/*
		 * At any given time, only one CPU thread can enter the
		 * turnstile to operate on the same stub process, including
		 * executing stub system calls (mmap and munmap).
		 */
		enter_turnstile(mm_id);

		/*
		 * When we are in time-travel mode, userspace can theoretically
		 * do a *lot* of work without being scheduled. The problem with
		 * this is that it will prevent kernel bookkeeping (primarily
		 * the RCU) from running and this can for example cause OOM
		 * situations.
		 *
		 * This code accounts a jiffie against the scheduling clock
		 * after the defined userspace iterations in the same thread.
		 * By doing so the situation is effectively prevented.
		 */
		if (time_travel_mode == TT_MODE_INFCPU ||
		    time_travel_mode == TT_MODE_EXTERNAL) {
#ifdef CONFIG_UML_MAX_USERSPACE_ITERATIONS
			if (CONFIG_UML_MAX_USERSPACE_ITERATIONS &&
			    unscheduled_userspace_iterations++ >
			    CONFIG_UML_MAX_USERSPACE_ITERATIONS) {
				tt_extra_sched_jiffies += 1;
				unscheduled_userspace_iterations = 0;
			}
#endif
		}

		time_travel_print_bc_msg();

		current_mm_sync();

		if (using_seccomp) {
			struct stub_data *proc_data = (void *) mm_id->stack;

			err = set_stub_state(regs, proc_data, singlestepping());
			if (err) {
				printk(UM_KERN_ERR "%s - failed to set regs: %d",
				       __func__, err);
				fatal_sigsegv();
			}

			/* Must have been reset by the syscall caller */
			if (proc_data->restart_wait != 0)
				panic("Programming error: Flag to only run syscalls in child was not cleared!");

			/* Mark pending syscalls for flushing */
			proc_data->syscall_data_len = mm_id->syscall_data_len;

			wait_stub_done_seccomp(mm_id, 0, 0);

			sig = proc_data->signal;

			if (sig == SIGTRAP && proc_data->err != 0) {
				printk(UM_KERN_ERR "%s - Error flushing stub syscalls",
				       __func__);
				syscall_stub_dump_error(mm_id);
				mm_id->syscall_data_len = proc_data->err;
				fatal_sigsegv();
			}

			mm_id->syscall_data_len = 0;
			mm_id->syscall_fd_num = 0;

			err = get_stub_state(regs, proc_data, NULL);
			if (err) {
				printk(UM_KERN_ERR "%s - failed to get regs: %d",
				       __func__, err);
				fatal_sigsegv();
			}

			if (proc_data->si_offset > sizeof(proc_data->sigstack) - sizeof(*si))
				panic("%s - Invalid siginfo offset from child", __func__);

			si = &si_local;
			memcpy(si, &proc_data->sigstack[proc_data->si_offset], sizeof(*si));

			regs->is_user = 1;

			/* Fill in ORIG_RAX and extract fault information */
			PT_SYSCALL_NR(regs->gp) = si->si_syscall;
			if (sig == SIGSEGV) {
				mcontext_t *mcontext = (void *)&proc_data->sigstack[proc_data->mctx_offset];

				GET_FAULTINFO_FROM_MC(regs->faultinfo, mcontext);
			}
		} else {
			int pid = mm_id->pid;

			/* Flush out any pending syscalls */
			err = syscall_stub_flush(mm_id);
			if (err) {
				if (err == -ENOMEM)
					report_enomem();

				printk(UM_KERN_ERR "%s - Error flushing stub syscalls: %d",
					__func__, -err);
				fatal_sigsegv();
			}

			/*
			 * This can legitimately fail if the process loads a
			 * bogus value into a segment register.  It will
			 * segfault and PTRACE_GETREGS will read that value
			 * out of the process.  However, PTRACE_SETREGS will
			 * fail.  In this case, there is nothing to do but
			 * just kill the process.
			 */
			if (put_host_regs(pid, regs->gp)) {
				printk(UM_KERN_ERR "%s - ptrace set regs failed, errno = %d\n",
				       __func__, errno);
				fatal_sigsegv();
			}

			if (put_fp_registers(pid, regs->fp)) {
				printk(UM_KERN_ERR "%s - ptrace set fp regs failed, errno = %d\n",
				       __func__, errno);
				fatal_sigsegv();
			}

			/*
			 * Without PTRACE_SYSEMU the guest is run under plain
			 * PTRACE_SYSCALL and the syscall is cancelled at the
			 * entry stop below, which reaches the same place: the
			 * call does not execute and the guest resumes past it
			 * with whatever UML puts in the return register.
			 */
			if (!have_ptrace_sysemu) {
				op = PTRACE_SYSCALL;
#ifdef UM_SYSCALL_TRAP_INSN
				/*
				 * Single-stepping still has to step. The one
				 * instruction that must not be stepped is the
				 * one that enters the kernel: stepping it would
				 * run the guest's syscall on the host. Take the
				 * syscall stop for that instruction instead, so
				 * it can be cancelled like any other.
				 */
				if (singlestepping() &&
				    !at_syscall_insn(pid, REGS_IP(regs->gp)))
					op = PTRACE_SINGLESTEP;
#endif
			} else if (singlestepping()) {
				op = PTRACE_SYSEMU_SINGLESTEP;
			} else {
				op = PTRACE_SYSEMU;
			}

			if (ptrace(op, pid, 0, 0)) {
				printk(UM_KERN_ERR "%s - ptrace continue failed, op = %d, errno = %d\n",
				       __func__, op, errno);
				fatal_sigsegv();
			}

			CATCH_EINTR(err = waitpid(pid, &status, WUNTRACED | __WALL));
			if (err < 0) {
				printk(UM_KERN_ERR "%s - wait failed, errno = %d\n",
				       __func__, errno);
				fatal_sigsegv();
			}

			regs->is_user = 1;
			if (get_host_regs(pid, regs->gp)) {
				printk(UM_KERN_ERR "%s - get_host_regs failed, errno = %d\n",
				       __func__, errno);
				fatal_sigsegv();
			}

			if (get_fp_registers(pid, regs->fp)) {
				printk(UM_KERN_ERR "%s -  get_fp_registers failed, errno = %d\n",
				       __func__, errno);
				fatal_sigsegv();
			}

#ifdef UM_SYSCALL_STOP_HIDES_REG
			/*
			 * One register is not visible at a syscall stop and
			 * cannot be written there either -- see
			 * UM_SYSCALL_STOP_HIDES_REG in <sysdep/ptrace_user.h>.
			 * Everything else has just been read correctly, so step
			 * off the stop and pick up that one register alone.
			 *
			 * The step runs no guest code: the syscall stays
			 * emulated away and the program counter does not move.
			 * It is still not a free stop to stand on -- it arrives
			 * as a forced SIGTRAP, so the host's signal path runs
			 * and clears the recorded syscall number, and would
			 * rewind the program counter if the first syscall
			 * argument happened to look like -ERESTARTSYS. Taking
			 * only the hidden register from here, and everything
			 * else from the syscall stop above, is immune to both:
			 * whatever the host did to the rest is overwritten by
			 * the register write that resumes this task.
			 *
			 * The other half of the reason to step is the write: the
			 * task is left parked on a stop where the full register
			 * set can be installed, which is what a guest thread
			 * switch needs and what a syscall stop silently refuses.
			 */
			if (WIFSTOPPED(status) &&
			    WSTOPSIG(status) == (SIGTRAP | 0x80)) {
				unsigned long hidden[MAX_REG_NR];
				int sstatus, tries = 0;

				/*
				 * On a host without PTRACE_SYSEMU this stop is
				 * an ordinary syscall-entry stop, so the call
				 * is about to run for real. Cancel it before
				 * stepping off the stop. The registers were
				 * read above, so the syscall number UML needs
				 * has already been taken and overwriting it
				 * here loses nothing.
					 *
					 * syscall_cancel_nr is -1, the documented
					 * "run nothing" value, unless the probe found
					 * the host refuses it -- in which case it names
					 * a harmless syscall to run in the guest call's
					 * place. See check_sysemu().
				 */
				if (!have_ptrace_sysemu &&
				    ptrace_set_syscall_nr(pid, syscall_cancel_nr)) {
					printk(UM_KERN_ERR "%s - failed to cancel a guest syscall, errno = %d\n",
					       __func__, errno);
					fatal_sigsegv();
				}

				while (1) {
					if (ptrace(PTRACE_SINGLESTEP, pid, 0, 0)) {
						printk(UM_KERN_ERR "%s - failed to step off a syscall stop, errno = %d\n",
						       __func__, errno);
						fatal_sigsegv();
					}

					CATCH_EINTR(err = waitpid(pid, &sstatus,
								  WUNTRACED | __WALL));
					if (err < 0) {
						printk(UM_KERN_ERR "%s - wait after step failed, errno = %d\n",
						       __func__, errno);
						fatal_sigsegv();
					}

					if (WIFSTOPPED(sstatus) &&
					    WSTOPSIG(sstatus) == SIGTRAP)
						break;

					/*
					 * The stub can be interrupted here, and
					 * wait_stub_done() tolerates the same two
					 * signals for the same reason. Neither is
					 * for the guest -- the stub installs no
					 * handler for either in ptrace mode, so
					 * delivering one would kill it -- and the
					 * step has not happened yet, so drop the
					 * signal and step again.
					 */
					if (WIFSTOPPED(sstatus) &&
					    ((1 << WSTOPSIG(sstatus)) & STUB_SIG_MASK) &&
					    ++tries < 16)
						continue;

					/*
					 * Anything else means the assumption
					 * above no longer holds. Fail loudly
					 * rather than run a guest on registers
					 * that are quietly wrong.
					 */
					printk(UM_KERN_ERR "%s - unexpected status 0x%x stepping off a syscall stop\n",
					       __func__, sstatus);
					fatal_sigsegv();
				}

				if (get_host_regs(pid, hidden)) {
					printk(UM_KERN_ERR "%s - get_host_regs after step failed, errno = %d\n",
					       __func__, errno);
					fatal_sigsegv();
				}

				regs->gp[UM_SYSCALL_STOP_HIDDEN_REG] =
					hidden[UM_SYSCALL_STOP_HIDDEN_REG];
			}
#endif


			if (WIFSTOPPED(status)) {
				sig = WSTOPSIG(status);

				/*
				 * These signal handlers need the si argument
				 * and SIGSEGV needs the faultinfo.
				 * The SIGIO and SIGALARM handlers which constitute
				 * the majority of invocations, do not use it.
				 */
				switch (sig) {
				case SIGSEGV:
					get_skas_faultinfo(pid,
							   &regs->faultinfo);
					fallthrough;
				case SIGTRAP:
				case SIGILL:
				case SIGBUS:
				case SIGFPE:
				case SIGWINCH:
					ptrace(PTRACE_GETSIGINFO, pid, 0,
					       (struct siginfo *)&si_local);
					si = &si_local;
					break;
				default:
					si = NULL;
					break;
				}
			} else {
				sig = 0;
			}
		}

		exit_turnstile(mm_id);

		UPT_SYSCALL_NR(regs) = -1; /* Assume: It's not a syscall */

		if (sig) {
			switch (sig) {
			case SIGSEGV:
				if (using_seccomp || PTRACE_FULL_FAULTINFO)
					(*sig_info[SIGSEGV])(SIGSEGV,
							     (struct siginfo *)si,
							     regs, NULL);
				else
					segv(regs->faultinfo, 0, 1, NULL, NULL);

				break;
			case SIGSYS:
				handle_syscall(regs);
				break;
			case SIGTRAP + 0x80:
				handle_trap(regs);
				break;
			case SIGTRAP:
				relay_signal(SIGTRAP, (struct siginfo *)si, regs, NULL);
				break;
			case SIGALRM:
				break;
			case SIGIO:
			case SIGILL:
			case SIGBUS:
			case SIGFPE:
			case SIGWINCH:
				block_signals_trace();
				(*sig_info[sig])(sig, (struct siginfo *)si, regs, NULL);
				unblock_signals_trace();
				break;
			default:
				printk(UM_KERN_ERR "%s - child stopped with signal %d\n",
				       __func__, sig);
				fatal_sigsegv();
			}
			interrupt_end();

			/* Avoid -ERESTARTSYS handling in host */
			if (PT_SYSCALL_NR_OFFSET != PT_SYSCALL_RET_OFFSET)
				PT_SYSCALL_NR(regs->gp) = -1;
		}
	}
}

void new_thread(void *stack, jmp_buf *buf, void (*handler)(void))
{
	(*buf)[0].JB_IP = (unsigned long) handler;
	/*
	 * How much to leave below the top of the stack is an ABI question, not a
	 * universal one: see ARCH_INIT_SP_RESERVE in <sysdep/archsetjmp.h>.
	 */
	(*buf)[0].JB_SP = (unsigned long) stack + UM_THREAD_SIZE -
		ARCH_INIT_SP_RESERVE;
}

#define INIT_JMP_NEW_THREAD 0
#define INIT_JMP_CALLBACK 1
#define INIT_JMP_HALT 2
#define INIT_JMP_REBOOT 3

void switch_threads(jmp_buf *me, jmp_buf *you)
{
	unscheduled_userspace_iterations = 0;

	if (UML_SETJMP(me) == 0)
		UML_LONGJMP(you, 1);
}

static jmp_buf initial_jmpbuf;

static __thread void (*cb_proc)(void *arg);
static __thread void *cb_arg;
static __thread jmp_buf *cb_back;

int start_idle_thread(void *stack, jmp_buf *switch_buf)
{
	int n;

	set_handler(SIGWINCH);

	/*
	 * Can't use UML_SETJMP or UML_LONGJMP here because they save
	 * and restore signals, with the possible side-effect of
	 * trying to handle any signals which came when they were
	 * blocked, which can't be done on this stack.
	 * Signals must be blocked when jumping back here and restored
	 * after returning to the jumper.
	 */
	n = setjmp(initial_jmpbuf);
	switch (n) {
	case INIT_JMP_NEW_THREAD:
		(*switch_buf)[0].JB_IP = (unsigned long) uml_finishsetup;
		(*switch_buf)[0].JB_SP = (unsigned long) stack +
			UM_THREAD_SIZE - ARCH_INIT_SP_RESERVE;
		break;
	case INIT_JMP_CALLBACK:
		(*cb_proc)(cb_arg);
		longjmp(*cb_back, 1);
		break;
	case INIT_JMP_HALT:
		kmalloc_ok = 0;
		return 0;
	case INIT_JMP_REBOOT:
		kmalloc_ok = 0;
		return 1;
	default:
		printk(UM_KERN_ERR "Bad sigsetjmp return in %s - %d\n",
		       __func__, n);
		fatal_sigsegv();
	}
	longjmp(*switch_buf, 1);

	/* unreachable */
	printk(UM_KERN_ERR "impossible long jump!");
	fatal_sigsegv();
	return 0;
}

void initial_thread_cb_skas(void (*proc)(void *), void *arg)
{
	jmp_buf here;

	cb_proc = proc;
	cb_arg = arg;
	cb_back = &here;

	initial_jmpbuf_lock();
	if (UML_SETJMP(&here) == 0)
		UML_LONGJMP(&initial_jmpbuf, INIT_JMP_CALLBACK);
	initial_jmpbuf_unlock();

	cb_proc = NULL;
	cb_arg = NULL;
	cb_back = NULL;
}

void halt_skas(void)
{
	initial_jmpbuf_lock();
	UML_LONGJMP(&initial_jmpbuf, INIT_JMP_HALT);
	/* unreachable */
}

static bool noreboot;

static int __init noreboot_cmd_param(char *str, int *add)
{
	*add = 0;
	noreboot = true;
	return 0;
}

__uml_setup("noreboot", noreboot_cmd_param,
"noreboot\n"
"    Rather than rebooting, exit always, akin to QEMU's -no-reboot option.\n"
"    This is useful if you're using CONFIG_PANIC_TIMEOUT in order to catch\n"
"    crashes in CI\n\n");

void reboot_skas(void)
{
	initial_jmpbuf_lock();
	UML_LONGJMP(&initial_jmpbuf, noreboot ? INIT_JMP_HALT : INIT_JMP_REBOOT);
	/* unreachable */
}
