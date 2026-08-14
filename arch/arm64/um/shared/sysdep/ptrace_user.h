/* SPDX-License-Identifier: GPL-2.0 */
#ifndef __SYSDEP_ARM64_PTRACE_USER_H
#define __SYSDEP_ARM64_PTRACE_USER_H

#include <generated/user_constants.h>

#define PT_OFFSET(r) ((r) * sizeof(long))

/*
 * On arm64 the syscall number is not part of the ptrace register view: x8 holds
 * it on entry but writing x8 does not change which syscall runs, and there is
 * no orig_x8. UML therefore keeps it in a synthesised slot which it fills from
 * the NT_ARM_SYSTEM_CALL regset (ptrace mode) or from x8 in the SIGSYS
 * mcontext (seccomp mode).
 */
#define PT_SYSCALL_NR(regs) ((regs)[HOST_SYSCALL_NR])
#define PT_SYSCALL_NR_OFFSET PT_OFFSET(HOST_SYSCALL_NR)

#define PT_SYSCALL_RET_OFFSET PT_OFFSET(HOST_X0)

#define REGS_IP_INDEX HOST_PC
#define REGS_SP_INDEX HOST_SP

#ifndef PTRACE_SYSEMU
#define PTRACE_SYSEMU 31
#endif
#ifndef PTRACE_SYSEMU_SINGLESTEP
#define PTRACE_SYSEMU_SINGLESTEP 32
#endif

#ifndef NT_ARM_SYSTEM_CALL
#define NT_ARM_SYSTEM_CALL 0x404
#endif
#ifndef NT_ARM_TLS
#define NT_ARM_TLS 0x401
#endif

#endif /* __SYSDEP_ARM64_PTRACE_USER_H */
