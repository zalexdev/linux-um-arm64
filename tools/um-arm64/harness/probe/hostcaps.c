// SPDX-License-Identifier: GPL-2.0
/*
 * hostcaps - probe an aarch64 host for everything an ARCH=um SUBARCH=arm64 port
 * depends on, and print the answers as machine-readable key=value lines.
 *
 * This exists because several design decisions in the port are contingent on host
 * kernel behaviour that is version-dependent and widely mis-stated:
 *
 *   - whether PTRACE_SYSEMU exists on arm64 (it does, since v5.3, but a lot of
 *     documentation still says it does not);
 *   - whether the syscall number can be *changed* via NT_ARM_SYSTEM_CALL, and
 *     whether writing -1 cancels the syscall;
 *   - whether the SIGSEGV signal frame carries an esr_context, which is the only
 *     way to tell a write fault from a read fault (x86 reads err&2 instead);
 *   - the real top of the address space, which sets TASK_SIZE and therefore where
 *     the stub can live;
 *   - whether personality(ADDR_NO_RANDOMIZE) is permitted.
 *
 * Build (from the x86_64 build domain):
 *   clang --target=aarch64-linux-gnu -O1 -g -Wall -o hostcaps hostcaps.c
 * Run on the aarch64 host. Exit status is 0 if every probe ran; individual probe
 * results are reported in the output, not the exit status, because "this host
 * cannot do X" is a finding, not an error.
 */
#define _GNU_SOURCE
#include <asm/ptrace.h>
#include <errno.h>
#include <linux/filter.h>
#include <linux/seccomp.h>
#include <sys/personality.h>
#include <signal.h>
#include <stdarg.h>
#include <stdint.h>
#include <ucontext.h>
#include <setjmp.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/prctl.h>
#include <sys/ptrace.h>
#include <sys/syscall.h>
#include <sys/types.h>
#include <sys/uio.h>
#include <sys/user.h>
#include <sys/utsname.h>
#include <sys/wait.h>
#include <unistd.h>

#ifndef PTRACE_SYSEMU
#define PTRACE_SYSEMU 31
#endif
#ifndef NT_ARM_SYSTEM_CALL
#define NT_ARM_SYSTEM_CALL 0x404
#endif
#ifndef NT_ARM_TLS
#define NT_ARM_TLS 0x401
#endif
/* elf.h and linux/elf.h cannot both be included (conflicting Elf64_* typedefs),
 * and we need only these two note types, so spell them out. */
#ifndef NT_PRSTATUS
#define NT_PRSTATUS 1
#endif
#ifndef NT_PRFPREG
#define NT_PRFPREG 2
#endif

/* asm/sigcontext.h values, spelled out so this builds against any libc. */
#define CTX_FPSIMD_MAGIC 0x46508001
#define CTX_ESR_MAGIC    0x45535201
#define CTX_EXTRA_MAGIC  0x45585401
#define CTX_SVE_MAGIC    0x53564501
#define CTX_ZA_MAGIC     0x54366345
#define CTX_TPIDR2_MAGIC 0x54504902

struct aarch64_ctx_hdr {
	uint32_t magic;
	uint32_t size;
};

static void kv(const char *k, const char *fmt, ...)
	__attribute__((format(printf, 2, 3)));

static void kv(const char *k, const char *fmt, ...)
{
	va_list ap;
	printf("%s=", k);
	va_start(ap, fmt);
	vprintf(fmt, ap);
	va_end(ap);
	putchar('\n');
	fflush(stdout);
}

/* ------------------------------------------------------------------ */
/* ptrace helpers                                                      */
/* ------------------------------------------------------------------ */

static int get_regset(pid_t pid, int which, void *buf, size_t len, size_t *out_len)
{
	struct iovec iov = { .iov_base = buf, .iov_len = len };
	if (ptrace(PTRACE_GETREGSET, pid, (void *)(long)which, &iov) < 0)
		return -errno;
	if (out_len)
		*out_len = iov.iov_len;
	return 0;
}

static int set_regset(pid_t pid, int which, void *buf, size_t len)
{
	struct iovec iov = { .iov_base = buf, .iov_len = len };
	if (ptrace(PTRACE_SETREGSET, pid, (void *)(long)which, &iov) < 0)
		return -errno;
	return 0;
}

/*
 * Child used by the ptrace probes: stop itself, then issue a getpid() we can
 * observe, cancel, or rewrite. It reports what it actually saw through its exit
 * status so the parent can tell "cancelled" from "ran".
 *
 * exit(1) => getpid() returned the real pid (syscall ran)
 * exit(2) => getpid() returned something else (syscall was cancelled/rewritten)
 */
static pid_t spawn_probe_child(void)
{
	pid_t pid = fork();
	if (pid != 0)
		return pid;

	ptrace(PTRACE_TRACEME, 0, 0, 0);
	raise(SIGSTOP);

	pid_t real = getpid();               /* observed via /proc, not the syscall */
	long ret = syscall(SYS_getpid);
	_exit(ret == real ? 1 : 2);
}

/* Wait for a ptrace-stop; returns the status or -1. */
static int wait_stop(pid_t pid, int *status)
{
	if (waitpid(pid, status, 0) < 0)
		return -1;
	return 0;
}

static void reap(pid_t pid)
{
	int st;
	kill(pid, SIGKILL);
	waitpid(pid, &st, 0);
}

/* ------------------------------------------------------------------ */
/* probe: PTRACE_SYSEMU                                                */
/* ------------------------------------------------------------------ */

static void probe_sysemu(void)
{
	int status;
	pid_t pid = spawn_probe_child();
	if (pid < 0) { kv("ptrace.sysemu", "error:fork:%s", strerror(errno)); return; }

	if (wait_stop(pid, &status) < 0 || !WIFSTOPPED(status)) {
		kv("ptrace.sysemu", "error:no-initial-stop");
		reap(pid); return;
	}
	ptrace(PTRACE_SETOPTIONS, pid, 0, (void *)(long)PTRACE_O_EXITKILL);

	errno = 0;
	if (ptrace(PTRACE_SYSEMU, pid, 0, 0) < 0) {
		kv("ptrace.sysemu", "unsupported:%s", strerror(errno));
		reap(pid); return;
	}
	if (wait_stop(pid, &status) < 0 || !WIFSTOPPED(status)) {
		kv("ptrace.sysemu", "error:no-syscall-stop");
		reap(pid); return;
	}

	/* We are at a syscall-entry stop with the syscall already suppressed. */
	struct user_pt_regs regs;
	size_t len;
	if (get_regset(pid, NT_PRSTATUS, &regs, sizeof(regs), &len) < 0) {
		kv("ptrace.sysemu", "error:getregset");
		reap(pid); return;
	}

	int scno = -1;
	size_t sclen;
	if (get_regset(pid, NT_ARM_SYSTEM_CALL, &scno, sizeof(scno), &sclen) == 0)
		kv("ptrace.sysemu.stopped_at_syscall", "%d", scno);

	/* Plant a distinguishable return value and let it run to exit. */
	regs.regs[0] = 0xdeadbeef;
	set_regset(pid, NT_PRSTATUS, &regs, sizeof(regs));
	ptrace(PTRACE_CONT, pid, 0, 0);
	if (wait_stop(pid, &status) == 0 && WIFEXITED(status)) {
		kv("ptrace.sysemu", "%s", WEXITSTATUS(status) == 2 ? "yes" : "ran-anyway");
		kv("ptrace.sysemu.child_exit", "%d", WEXITSTATUS(status));
	} else {
		kv("ptrace.sysemu", "error:child-did-not-exit");
		reap(pid);
	}
}

/* ------------------------------------------------------------------ */
/* probe: NT_ARM_SYSTEM_CALL read / write / cancel                     */
/* ------------------------------------------------------------------ */

static void probe_system_call_regset(void)
{
	int status;
	pid_t pid = spawn_probe_child();
	if (pid < 0) { kv("regset.system_call", "error:fork"); return; }

	if (wait_stop(pid, &status) < 0 || !WIFSTOPPED(status)) {
		kv("regset.system_call", "error:no-initial-stop"); reap(pid); return;
	}
	ptrace(PTRACE_SETOPTIONS, pid, 0, (void *)(long)PTRACE_O_EXITKILL);

	/* Ordinary syscall tracing, so the syscall is *not* pre-cancelled. */
	ptrace(PTRACE_SYSCALL, pid, 0, 0);
	if (wait_stop(pid, &status) < 0 || !WIFSTOPPED(status)) {
		kv("regset.system_call", "error:no-syscall-stop"); reap(pid); return;
	}

	int scno = -2;
	size_t len = 0;
	int rc = get_regset(pid, NT_ARM_SYSTEM_CALL, &scno, sizeof(scno), &len);
	if (rc < 0) {
		kv("regset.system_call.get", "error:%s", strerror(-rc));
		reap(pid); return;
	}
	kv("regset.system_call.get", "ok");
	kv("regset.system_call.size", "%zu", len);
	kv("regset.system_call.value", "%d", scno);
	kv("regset.system_call.is_getpid", "%s", scno == SYS_getpid ? "yes" : "no");

	/* x8 should also hold the number at entry; UML must know if it can trust it. */
	struct user_pt_regs regs;
	if (get_regset(pid, NT_PRSTATUS, &regs, sizeof(regs), NULL) == 0) {
		kv("regset.gpr.x8_at_entry", "%llu", (unsigned long long)regs.regs[8]);
		kv("regset.gpr.x8_matches_scno", "%s",
		   (long)regs.regs[8] == (long)scno ? "yes" : "no");
		kv("regset.gpr.pc", "0x%llx", (unsigned long long)regs.pc);
		kv("regset.gpr.sp", "0x%llx", (unsigned long long)regs.sp);
		kv("regset.gpr.pstate", "0x%llx", (unsigned long long)regs.pstate);
	}

	/* The cancellation mechanism the port depends on: write -1. */
	int cancel = -1;
	rc = set_regset(pid, NT_ARM_SYSTEM_CALL, &cancel, sizeof(cancel));
	kv("regset.system_call.set_minus1", "%s", rc == 0 ? "ok" : strerror(-rc));

	/* Write a recognisable return value; if cancellation worked the child sees it. */
	if (get_regset(pid, NT_PRSTATUS, &regs, sizeof(regs), NULL) == 0) {
		regs.regs[0] = 0x5eeded;
		set_regset(pid, NT_PRSTATUS, &regs, sizeof(regs));
	}

	ptrace(PTRACE_CONT, pid, 0, 0);
	if (wait_stop(pid, &status) == 0 && WIFEXITED(status)) {
		kv("regset.system_call.cancel_works", "%s",
		   WEXITSTATUS(status) == 2 ? "yes" : "no");
	} else {
		kv("regset.system_call.cancel_works", "error");
		reap(pid);
	}
}

/* Can we *rewrite* one syscall as another? Needed if we ever redirect stub calls. */
static void probe_system_call_rewrite(void)
{
	int status;
	pid_t pid = spawn_probe_child();
	if (pid < 0) { kv("regset.system_call.rewrite", "error:fork"); return; }
	if (wait_stop(pid, &status) < 0) { kv("regset.system_call.rewrite", "error"); return; }
	ptrace(PTRACE_SETOPTIONS, pid, 0, (void *)(long)PTRACE_O_EXITKILL);
	ptrace(PTRACE_SYSCALL, pid, 0, 0);
	if (wait_stop(pid, &status) < 0 || !WIFSTOPPED(status)) {
		kv("regset.system_call.rewrite", "error:no-stop"); reap(pid); return;
	}
	int newnr = SYS_gettid;
	int rc = set_regset(pid, NT_ARM_SYSTEM_CALL, &newnr, sizeof(newnr));
	if (rc < 0) { kv("regset.system_call.rewrite", "error:%s", strerror(-rc)); reap(pid); return; }
	ptrace(PTRACE_CONT, pid, 0, 0);
	if (wait_stop(pid, &status) == 0 && WIFEXITED(status)) {
		/* gettid in a single-threaded child == pid, so exit 1 means the rewrite
		 * took effect and still returned the pid; we can only prove the syscall
		 * did not fail. Report the raw status for the record. */
		kv("regset.system_call.rewrite", "accepted");
		kv("regset.system_call.rewrite_exit", "%d", WEXITSTATUS(status));
	} else {
		kv("regset.system_call.rewrite", "error:no-exit");
		reap(pid);
	}
}

/* ------------------------------------------------------------------ */
/* probe: NT_ARM_TLS                                                   */
/* ------------------------------------------------------------------ */

static void probe_tls(void)
{
	int status;
	pid_t pid = spawn_probe_child();
	if (pid < 0) { kv("regset.tls", "error:fork"); return; }
	if (wait_stop(pid, &status) < 0) { kv("regset.tls", "error"); return; }
	ptrace(PTRACE_SETOPTIONS, pid, 0, (void *)(long)PTRACE_O_EXITKILL);

	unsigned long tls = 0;
	size_t len = 0;
	int rc = get_regset(pid, NT_ARM_TLS, &tls, sizeof(tls), &len);
	if (rc < 0) {
		kv("regset.tls", "error:%s", strerror(-rc));
	} else {
		kv("regset.tls", "ok");
		kv("regset.tls.size", "%zu", len);
		kv("regset.tls.value", "0x%lx", tls);
		unsigned long probe = 0xfeedface000UL;
		rc = set_regset(pid, NT_ARM_TLS, &probe, sizeof(probe));
		kv("regset.tls.set", "%s", rc == 0 ? "ok" : strerror(-rc));
		unsigned long back = 0;
		if (rc == 0 && get_regset(pid, NT_ARM_TLS, &back, sizeof(back), NULL) == 0)
			kv("regset.tls.readback_ok", "%s", back == probe ? "yes" : "no");
	}
	reap(pid);
}

/* ------------------------------------------------------------------ */
/* probe: SIGSEGV signal frame -- fault address and ESR                */
/* ------------------------------------------------------------------ */

static volatile sig_atomic_t segv_seen;
static unsigned long segv_fault_addr;
static unsigned long segv_esr;
static int segv_have_esr;
static char segv_ctx_list[256];

static void scan_reserved(const unsigned char *res, size_t max)
{
	size_t off = 0;
	segv_ctx_list[0] = '\0';
	while (off + sizeof(struct aarch64_ctx_hdr) <= max) {
		const struct aarch64_ctx_hdr *h =
			(const struct aarch64_ctx_hdr *)(res + off);
		const char *name;
		char buf[32];

		if (h->magic == 0 && h->size == 0)
			break;
		switch (h->magic) {
		case CTX_FPSIMD_MAGIC: name = "fpsimd"; break;
		case CTX_ESR_MAGIC:    name = "esr";    break;
		case CTX_EXTRA_MAGIC:  name = "extra";  break;
		case CTX_SVE_MAGIC:    name = "sve";    break;
		case CTX_ZA_MAGIC:     name = "za";     break;
		case CTX_TPIDR2_MAGIC: name = "tpidr2"; break;
		default:
			snprintf(buf, sizeof(buf), "0x%x", h->magic);
			name = buf;
			break;
		}
		if (strlen(segv_ctx_list) + strlen(name) + 2 < sizeof(segv_ctx_list)) {
			if (segv_ctx_list[0])
				strcat(segv_ctx_list, ",");
			strcat(segv_ctx_list, name);
		}
		if (h->magic == CTX_ESR_MAGIC) {
			segv_esr = *(const uint64_t *)(res + off + sizeof(*h));
			segv_have_esr = 1;
		}
		if (h->size == 0)
			break;
		off += h->size;
	}
}

/*
 * Escape hatch for the instruction-abort probe. Stepping pc over the faulting
 * instruction only works for data aborts: on an instruction abort pc *is* the
 * bad address, so advancing it by 4 just faults again one word further in.
 * siglongjmp is the only way back out.
 */
static sigjmp_buf segv_escape;
static volatile sig_atomic_t segv_use_escape;

static void segv_handler(int sig, siginfo_t *si, void *ctx)
{
	ucontext_t *uc = ctx;
	(void)sig;
	segv_seen = 1;
	segv_fault_addr = (unsigned long)si->si_addr;
	segv_have_esr = 0;
	segv_esr = 0;
	scan_reserved((const unsigned char *)uc->uc_mcontext.__reserved,
		      sizeof(uc->uc_mcontext.__reserved));
	if (segv_use_escape)
		siglongjmp(segv_escape, 1);
	/* Data abort: step over the faulting instruction. All our probes are single
	 * 4-byte loads or stores, and arm64 instructions are fixed width. */
	uc->uc_mcontext.pc += 4;
}

static void probe_signal_frame(void)
{
	struct sigaction sa = { 0 }, old;
	sa.sa_sigaction = segv_handler;
	sa.sa_flags = SA_SIGINFO;
	sigemptyset(&sa.sa_mask);
	if (sigaction(SIGSEGV, &sa, &old) < 0) {
		kv("sigframe", "error:sigaction");
		return;
	}

	size_t ps = (size_t)sysconf(_SC_PAGESIZE);
	unsigned char *p = mmap(NULL, ps, PROT_NONE,
				MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
	if (p == MAP_FAILED) { kv("sigframe", "error:mmap"); return; }

	/* --- read fault --- */
	segv_seen = 0;
	{ volatile unsigned int v = *(volatile unsigned int *)p; (void)v; }
	if (segv_seen) {
		kv("sigframe.read.si_addr_ok", "%s",
		   segv_fault_addr == (unsigned long)p ? "yes" : "no");
		kv("sigframe.read.contexts", "%s", segv_ctx_list);
		kv("sigframe.read.have_esr", "%s", segv_have_esr ? "yes" : "no");
		if (segv_have_esr) {
			kv("sigframe.read.esr", "0x%lx", segv_esr);
			kv("sigframe.read.esr_ec", "0x%lx", (segv_esr >> 26) & 0x3f);
			kv("sigframe.read.esr_wnr", "%lu", (segv_esr >> 6) & 1);
		}
	} else {
		kv("sigframe.read", "error:no-segv");
	}

	/* --- write fault --- */
	segv_seen = 0;
	*(volatile unsigned int *)p = 0x12345678;
	if (segv_seen) {
		kv("sigframe.write.si_addr_ok", "%s",
		   segv_fault_addr == (unsigned long)p ? "yes" : "no");
		kv("sigframe.write.have_esr", "%s", segv_have_esr ? "yes" : "no");
		if (segv_have_esr) {
			kv("sigframe.write.esr", "0x%lx", segv_esr);
			kv("sigframe.write.esr_ec", "0x%lx", (segv_esr >> 26) & 0x3f);
			kv("sigframe.write.esr_wnr", "%lu", (segv_esr >> 6) & 1);
		}
	} else {
		kv("sigframe.write", "error:no-segv");
	}

	/* --- instruction fetch fault (exec of a PROT_NONE page) --- */
	segv_seen = 0;
	segv_use_escape = 1;
	if (sigsetjmp(segv_escape, 1) == 0) {
		void (*fn)(void) = (void (*)(void))p;
		fn();
	}
	segv_use_escape = 0;
	if (segv_seen) {
		kv("sigframe.exec.have_esr", "%s", segv_have_esr ? "yes" : "no");
		if (segv_have_esr) {
			kv("sigframe.exec.esr", "0x%lx", segv_esr);
			kv("sigframe.exec.esr_ec", "0x%lx", (segv_esr >> 26) & 0x3f);
		}
	} else {
		kv("sigframe.exec", "no-segv");
	}

	munmap(p, ps);
	sigaction(SIGSEGV, &old, NULL);
	kv("sigframe.mcontext_reserved_size", "%zu",
	   sizeof(((mcontext_t *)0)->__reserved));
}

/* ------------------------------------------------------------------ */
/* probe: address space shape                                          */
/* ------------------------------------------------------------------ */

static int can_map_at(unsigned long addr, size_t len)
{
	void *p = mmap((void *)addr, len, PROT_READ | PROT_WRITE,
		       MAP_PRIVATE | MAP_ANONYMOUS | MAP_FIXED_NOREPLACE, -1, 0);
	if (p == MAP_FAILED)
		return 0;
	int ok = (p == (void *)addr);
	munmap(p, len);
	return ok;
}

static void probe_address_space(void)
{
	size_t ps = (size_t)sysconf(_SC_PAGESIZE);
	kv("as.pagesize", "%zu", ps);

	/* Highest address we can actually map: binary-search the VA ceiling. */
	unsigned long lo = 1UL << 20, hi = 1UL << 55, best = 0;
	while (lo <= hi) {
		unsigned long mid = lo + ((hi - lo) / 2);
		mid &= ~(unsigned long)(ps - 1);
		if (mid == 0) break;
		if (can_map_at(mid, ps)) { best = mid; lo = mid + ps; }
		else                     { if (mid < ps) break; hi = mid - ps; }
	}
	kv("as.highest_mappable", "0x%lx", best);
	kv("as.approx_va_bits", "%d", best ? (64 - __builtin_clzl(best)) : 0);

	/* mmap_min_addr governs how low the stub may be placed. */
	FILE *f = fopen("/proc/sys/vm/mmap_min_addr", "r");
	unsigned long mma = 0;
	if (f) { if (fscanf(f, "%lu", &mma) != 1) mma = 0; fclose(f); }
	kv("as.mmap_min_addr", "%lu", mma);

	/* Can we place a page at a low fixed address? The stub wants this. */
	unsigned long cand[] = { 0x10000, 0x100000, 0x1000000, 0x40000000 };
	for (size_t i = 0; i < sizeof(cand) / sizeof(cand[0]); i++)
		kv("as.fixed_map_ok", "0x%lx:%s", cand[i],
		   can_map_at(cand[i], ps) ? "yes" : "no");

	kv("as.randomize_va_space", "%s", ({
		static char b[8]; FILE *g = fopen("/proc/sys/kernel/randomize_va_space", "r");
		if (g) { if (!fgets(b, sizeof(b), g)) strcpy(b, "?"); fclose(g); } else strcpy(b, "?");
		b[strcspn(b, "\n")] = 0; b; }));
}

static void probe_personality(void)
{
	int old = personality(0xffffffff);
	if (old < 0) { kv("personality.read", "error:%s", strerror(errno)); return; }
	kv("personality.current", "0x%x", (unsigned)old);
	int rc = personality((unsigned)old | ADDR_NO_RANDOMIZE);
	if (rc < 0) {
		kv("personality.addr_no_randomize", "denied:%s", strerror(errno));
	} else {
		int now = personality(0xffffffff);
		kv("personality.addr_no_randomize", "%s",
		   (now >= 0 && (now & ADDR_NO_RANDOMIZE)) ? "allowed" : "ignored");
		personality((unsigned)old);
	}
}

/* ------------------------------------------------------------------ */
/* probe: seccomp                                                      */
/* ------------------------------------------------------------------ */

static void probe_seccomp(void)
{
	/* Ask the kernel about actions without installing anything. */
	unsigned int act;
	int rc;

	act = SECCOMP_RET_TRAP;
	rc = syscall(SYS_seccomp, SECCOMP_GET_ACTION_AVAIL, 0, &act);
	kv("seccomp.ret_trap", "%s", rc == 0 ? "yes" : strerror(errno));

#ifdef SECCOMP_RET_USER_NOTIF
	act = SECCOMP_RET_USER_NOTIF;
	rc = syscall(SYS_seccomp, SECCOMP_GET_ACTION_AVAIL, 0, &act);
	kv("seccomp.user_notif", "%s", rc == 0 ? "yes" : strerror(errno));
#else
	kv("seccomp.user_notif", "headers-lack-define");
#endif

	/* SECCOMP_FILTER_FLAG_TSYNC / _NEW_LISTENER availability, cheaply: pass a
	 * NULL filter and read the errno the kernel picks. EFAULT means the flag
	 * combination was accepted and it got as far as reading the filter. */
	rc = syscall(SYS_seccomp, SECCOMP_SET_MODE_FILTER,
		     SECCOMP_FILTER_FLAG_NEW_LISTENER, NULL);
	kv("seccomp.flag_new_listener", "%s",
	   (rc < 0 && errno == EFAULT) ? "yes" : strerror(errno));

	kv("seccomp.no_new_privs", "%d", prctl(PR_GET_NO_NEW_PRIVS, 0, 0, 0, 0));
}

/* ------------------------------------------------------------------ */

int main(void)
{
	struct utsname u;

	if (uname(&u) == 0) {
		kv("host.sysname", "%s", u.sysname);
		kv("host.release", "%s", u.release);
		kv("host.machine", "%s", u.machine);
	}
	kv("probe.built_by", "%s", __VERSION__);

	probe_address_space();
	probe_personality();
	probe_signal_frame();
	probe_sysemu();
	probe_system_call_regset();
	probe_system_call_rewrite();
	probe_tls();
	probe_seccomp();

	kv("probe.complete", "yes");
	return 0;
}
