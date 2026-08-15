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

/*
 * arm64 makes one general-purpose register unusable at a ptrace syscall stop.
 *
 * To tell a tracer whether a stop came from syscall entry or syscall exit, the
 * arm64 kernel overwrites a register in the tracee with the direction -- x7 for
 * AArch64, r12 for AArch32 -- and puts the tracee's own value back when the stop
 * ends. ptrace_save_reg() in arch/arm64/kernel/ptrace.c spells out the
 * consequences: writes by the tracer to that register during the stop are
 * discarded, and the real value is not available while stopped. Confirmed
 * against a live 6.12 host: x7 reads back as 0 (PTRACE_SYSCALL_ENTER) rather
 * than the value the tracee put there, and a write of 0xdeadbeef is gone by the
 * next stop.
 *
 * x86 has no equivalent, so nothing in UML expected it, and the result was a
 * guest-visible data corruption rather than a missing feature. Every thread of a
 * guest mm shares one stub process, so UML installs a thread's registers into
 * that stub on each switch; x7 silently kept whichever thread ran last, and the
 * poisoned read left a zero in UML's own copy. In a threaded guest that shows up
 * as a pointer held in x7 turning into zero mid-loop -- liblzma's LZ dictionary
 * base, in the case that found this, so dpkg-deb faulted at what was really a
 * dictionary offset.
 *
 * The escape, measured the same way: resuming a PTRACE_SYSEMU entry stop with
 * PTRACE_SINGLESTEP lands on a pseudo-step SIGTRAP where the
 * register is both readable and writable, without executing a single guest
 * instruction -- the syscall stays emulated away and the program counter does
 * not move. See userspace() in arch/um/os-Linux/skas/process.c.
 *
 * SECCOMP mode is unaffected: its traps are ordinary signals, and the arm64
 * comment above notes that seccomp and pseudo-step traps nobble nothing.
 */
#define UM_SYSCALL_STOP_HIDES_REG 1
#define UM_SYSCALL_STOP_HIDDEN_REG HOST_X7

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
