/* SPDX-License-Identifier: GPL-2.0 */
#ifndef __X86_UM_SYSDEP_ARCHSETJMP_H
#define __X86_UM_SYSDEP_ARCHSETJMP_H

#ifdef __i386__
#include "archsetjmp_32.h"
#else
#include "archsetjmp_64.h"
#endif

unsigned long get_thread_reg(int reg, jmp_buf *buf);


/*
 * Bytes to leave below the top of a fresh kernel stack.
 *
 * The x86 SysV ABI wants the stack pointer congruent to 8 mod 16 *at function
 * entry*, because `call` has just pushed an 8-byte return address. Reserving one
 * word here reproduces that state, so the callee's prologue lands on a 16-byte
 * boundary.
 */
#define ARCH_INIT_SP_RESERVE	sizeof(void *)

#endif /* __X86_UM_SYSDEP_ARCHSETJMP_H */
