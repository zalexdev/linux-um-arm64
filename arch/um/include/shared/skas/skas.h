/* SPDX-License-Identifier: GPL-2.0 */
/*
 * Copyright (C) 2002 - 2007 Jeff Dike (jdike@{addtoit,linux.intel}.com)
 */

#ifndef __SKAS_H
#define __SKAS_H

#include <sysdep/ptrace.h>

extern int using_seccomp;

/*
 * Whether the host implements PTRACE_SYSEMU. Where it does not, UML builds the
 * same semantics out of PTRACE_SYSCALL and a syscall cancellation; see
 * check_sysemu(). Set once at boot, before any stub exists.
 */
extern int have_ptrace_sysemu;
extern int syscall_cancel_nr;

extern void new_thread_handler(void);
extern void handle_syscall(struct uml_pt_regs *regs);
extern unsigned long current_stub_stack(void);
extern struct mm_id *current_mm_id(void);
extern void current_mm_sync(void);
void initial_jmpbuf_lock(void);
void initial_jmpbuf_unlock(void);

#endif
