// SPDX-License-Identifier: GPL-2.0
/*
 * Host register access for ARCH=um SUBARCH=arm64.
 *
 * arm64 has no PTRACE_GETREGS/PTRACE_SETREGS: everything goes through
 * PTRACE_GETREGSET/PTRACE_SETREGSET with an explicit note type. That is not
 * merely a spelling difference, because the syscall number is not part of the
 * primary register set at all -- it lives in its own NT_ARM_SYSTEM_CALL regset,
 * and it is the only way to change or cancel a syscall. x86 gets the equivalent
 * for free as orig_ax inside user_regs_struct.
 */
#include <errno.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ptrace.h>
#include <sys/uio.h>
#include <sys/mman.h>
#include <linux/elf.h>
#include <longjmp.h>
#include <sysdep/ptrace.h>
#include <sysdep/ptrace_user.h>
#include <registers.h>

static unsigned long ptrace_fpregset;
unsigned long host_fp_size;

static int getregset(int pid, int which, void *buf, size_t len, size_t *out_len)
{
	struct iovec iov = { .iov_base = buf, .iov_len = len };

	if (ptrace(PTRACE_GETREGSET, pid, (void *)(long)which, &iov) < 0)
		return -errno;
	if (out_len)
		*out_len = iov.iov_len;
	return 0;
}

static int setregset(int pid, int which, void *buf, size_t len)
{
	struct iovec iov = { .iov_base = buf, .iov_len = len };

	if (ptrace(PTRACE_SETREGSET, pid, (void *)(long)which, &iov) < 0)
		return -errno;
	return 0;
}

int get_fp_registers(int pid, unsigned long *regs)
{
	return getregset(pid, ptrace_fpregset, regs, host_fp_size, NULL);
}

int put_fp_registers(int pid, unsigned long *regs)
{
	return setregset(pid, ptrace_fpregset, regs, host_fp_size);
}

/*
 * Read the guest's general-purpose registers.
 *
 * Only the first UM_FRAME_SIZE bytes of gp[] correspond to the host's
 * NT_PRSTATUS regset; the two slots above that are synthesised here:
 *
 *   HOST_SYSCALL_NR  read from NT_ARM_SYSTEM_CALL. x8 also holds the number at a
 *                    syscall-entry stop, but relying on x8 would be wrong the
 *                    moment the guest is stopped anywhere else, and x8 is not
 *                    writable as a syscall selector in any case.
 *
 *   HOST_ORIG_X0     x0 as it stands now. UML only inspects registers at a
 *                    syscall-entry stop (PTRACE_SYSEMU stops before the syscall
 *                    runs, and the seccomp path traps at the SVC), so x0 still
 *                    holds the first argument rather than a return value. This
 *                    is what makes a later UPT_RESTART_SYSCALL able to put the
 *                    argument back after the host has overwritten x0 with
 *                    -ERESTARTSYS.
 */
int get_host_regs(int pid, unsigned long *regs)
{
	unsigned long tls = 0;
	int scno = -1;
	int err;

	err = getregset(pid, NT_PRSTATUS, regs, UM_FRAME_SIZE, NULL);
	if (err)
		return err;

	regs[HOST_ORIG_X0] = regs[HOST_X0];

	/*
	 * Not every stop is a syscall stop, and the host refuses the regset
	 * outside one. That is not an error: leave the slot as "no syscall".
	 */
	if (getregset(pid, NT_ARM_SYSTEM_CALL, &scno, sizeof(scno), NULL))
		scno = -1;
	regs[HOST_SYSCALL_NR] = (unsigned long)(long)scno;

	/*
	 * TPIDR_EL0. Always available, unlike the syscall regset, because it is
	 * ordinary thread state rather than something meaningful only at a
	 * syscall stop.
	 */
	if (getregset(pid, NT_ARM_TLS, &tls, sizeof(tls), NULL))
		tls = 0;
	regs[HOST_TLS] = tls;

	return 0;
}

/*
 * Write the guest's general-purpose registers back.
 *
 * The syscall number is written separately and only when it actually changed,
 * because NT_ARM_SYSTEM_CALL is writable only at a syscall-entry stop and
 * poking it unconditionally would fail on every other stop. A failure here is
 * therefore reported but is not fatal to the GPR write that already succeeded.
 */
int put_host_regs(int pid, unsigned long *regs)
{
	int scno = (int)(long)regs[HOST_SYSCALL_NR];
	int cur = -1;
	int err;

	err = setregset(pid, NT_PRSTATUS, regs, UM_FRAME_SIZE);
	if (err)
		return err;

	/*
	 * TPIDR_EL0 is not part of NT_PRSTATUS on arm64, so guest TLS has to be
	 * pushed separately or a switch between two guest threads in the same
	 * stub process would leave the previous thread's TLS in place.
	 */
	setregset(pid, NT_ARM_TLS, &regs[HOST_TLS], sizeof(unsigned long));

	if (getregset(pid, NT_ARM_SYSTEM_CALL, &cur, sizeof(cur), NULL))
		return 0;
	if (cur == scno)
		return 0;

	/*
	 * Best effort: outside a syscall-entry stop the host rejects this, and
	 * the value is meaningless there anyway. Returning the error would make
	 * callers treat a perfectly good GPR write as a failure -- for instance
	 * do_syscall_stub(), which sets up the stub's registers at a SIGTRAP
	 * stop and would panic.
	 */
	setregset(pid, NT_ARM_SYSTEM_CALL, &scno, sizeof(scno));
	return 0;
}


/*
 * The syscall number is a regset of its own on arm64. x8 holds it at entry, but
 * writing x8 does not change which syscall runs -- the kernel latched the number
 * before the ptrace stop -- so NT_ARM_SYSTEM_CALL is the only lever, and writing
 * -1 through it is how a syscall is cancelled.
 */
long ptrace_get_syscall_nr(int pid)
{
	int scno = -1;
	int err;

	err = getregset(pid, NT_ARM_SYSTEM_CALL, &scno, sizeof(scno), NULL);
	if (err) {
		errno = -err;
		return -1;
	}
	errno = 0;
	return scno;
}

int ptrace_set_syscall_nr(int pid, long nr)
{
	int scno = (int)nr;

	return setregset(pid, NT_ARM_SYSTEM_CALL, &scno, sizeof(scno));
}

int ptrace_set_syscall_ret(int pid, long val)
{
	unsigned long regs[MAX_REG_NR];
	int err;

	/*
	 * The return value is just x0, but arm64 has no PTRACE_POKEUSER, so the
	 * whole GPR set has to be read back, modified and rewritten.
	 */
	err = getregset(pid, NT_PRSTATUS, regs, UM_FRAME_SIZE, NULL);
	if (err)
		return err;

	regs[HOST_X0] = (unsigned long)val;

	return setregset(pid, NT_PRSTATUS, regs, UM_FRAME_SIZE);
}

const char *ptrace_reg_name(int idx)
{
	static const char * const xname[] = {
		"X0",  "X1",  "X2",  "X3",  "X4",  "X5",  "X6",  "X7",
		"X8",  "X9",  "X10", "X11", "X12", "X13", "X14", "X15",
		"X16", "X17", "X18", "X19", "X20", "X21", "X22", "X23",
		"X24", "X25", "X26", "X27", "X28", "X29", "X30",
	};

	if (idx >= HOST_X0 && idx <= HOST_X30)
		return xname[idx - HOST_X0];
	if (idx == HOST_SP)
		return "SP";
	if (idx == HOST_PC)
		return "PC";
	if (idx == HOST_PSTATE)
		return "PSTATE";
	if (idx == HOST_ORIG_X0)
		return "ORIG_X0";
	if (idx == HOST_SYSCALL_NR)
		return "SYSCALL_NR";
	return "";
}

/*
 * Size the FP regset once, against a live stub process.
 *
 * NT_ARM_SVE is tried first: on a host with SVE the FPSIMD registers are the
 * bottom 128 bits of the Z registers, so saving only NT_PRFPREG would silently
 * drop guest state. NT_PRFPREG is the fallback and is always present.
 *
 * The buffer is deliberately oversized and the kernel reports back how much it
 * actually wrote, which is the same trick x86 uses for XSTATE.
 */
int arch_init_registers(int pid)
{
	size_t probe_len = 2 * 1024 * 1024;
	size_t got = 0;
	void *buf;
	int ret;

	buf = mmap(NULL, probe_len, PROT_READ | PROT_WRITE,
		   MAP_ANONYMOUS | MAP_PRIVATE, -1, 0);
	if (buf == MAP_FAILED)
		return -ENOMEM;

	ptrace_fpregset = NT_ARM_SVE;
	ret = getregset(pid, ptrace_fpregset, buf, probe_len, &got);
	if (ret) {
		ptrace_fpregset = NT_PRFPREG;
		got = 0;
		ret = getregset(pid, ptrace_fpregset, buf, probe_len, &got);
	}

	munmap(buf, probe_len);

	host_fp_size = got;

	return ret;
}

unsigned long get_thread_reg(int reg, jmp_buf *buf)
{
	switch (reg) {
	case HOST_PC:
		/*
		 * A longjmp resumes at the saved return address, so for a
		 * jmp_buf the link register *is* the instruction pointer.
		 */
		return buf[0]->__x30;
	case HOST_SP:
		return buf[0]->__sp;
	case HOST_X29:
		return buf[0]->__x29;
	default:
		printk(UM_KERN_ERR "get_thread_reg - unknown register %d\n",
		       reg);
		return 0;
	}
}
