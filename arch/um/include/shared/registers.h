/* SPDX-License-Identifier: GPL-2.0 */
/*
 * Copyright (C) 2004 PathScale, Inc
 */

#ifndef __REGISTERS_H
#define __REGISTERS_H

#include <sysdep/ptrace.h>

/*
 * Transfer the general-purpose register file to/from a stub process.
 *
 * These used to be open-coded PTRACE_GETREGS/PTRACE_SETREGS calls. Those
 * requests are x86 (and a few other architectures) only: arm64 implements the
 * regset interface exclusively, and there is no PTRACE_GETREGS to call. The
 * subarch also uses these to fill in any register state that does not live in
 * the host's primary regset -- on arm64 the syscall number, which is reachable
 * only through NT_ARM_SYSTEM_CALL.
 */
extern int get_host_regs(int pid, unsigned long *regs);
extern int put_host_regs(int pid, unsigned long *regs);

/* Human-readable name for gp[] index @idx, or "" -- used by the crash dump. */
extern const char *ptrace_reg_name(int idx);

/*
 * Inspect and rewrite the syscall a stopped tracee is about to make.
 *
 * These were open-coded as PTRACE_PEEKUSER/PTRACE_POKEUSER at
 * PT_SYSCALL_NR_OFFSET. That works on x86, where the syscall number lives in
 * orig_ax inside the ptrace user area. arm64 implements neither request, and
 * its syscall number is not in the user area at all: it is reachable only
 * through the NT_ARM_SYSTEM_CALL regset, which is also the only way to cancel a
 * syscall (by writing -1).
 *
 * ptrace_get_syscall_nr() returns the number, or -1 with errno set.
 * The setters return 0 or a negative errno.
 */
extern long ptrace_get_syscall_nr(int pid);
extern int ptrace_set_syscall_nr(int pid, long nr);
extern int ptrace_set_syscall_ret(int pid, long val);

extern int init_pid_registers(int pid);
extern void get_safe_registers(unsigned long *regs, unsigned long *fp_regs);
extern int get_fp_registers(int pid, unsigned long *regs);
extern int put_fp_registers(int pid, unsigned long *regs);

#endif
