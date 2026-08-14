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

extern int init_pid_registers(int pid);
extern void get_safe_registers(unsigned long *regs, unsigned long *fp_regs);
extern int get_fp_registers(int pid, unsigned long *regs);
extern int put_fp_registers(int pid, unsigned long *regs);

#endif
