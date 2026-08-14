/* SPDX-License-Identifier: GPL-2.0 */
/* 
 * Copyright (C) 2000 - 2007 Jeff Dike (jdike@{addtoit,linux.intel}.com)
 */

#ifndef __PTRACE_USER_H__
#define __PTRACE_USER_H__

#include <sys/ptrace.h>
#include <sysdep/ptrace_user.h>

/*
 * ptrace_getregs()/ptrace_setregs() used to live here as a second, identical
 * spelling of get_host_regs()/put_host_regs() in <registers.h>. Having two
 * names for one operation meant every subarch had to implement both; there is
 * now one, and it is the one that can express arm64's split regsets.
 */

#endif
