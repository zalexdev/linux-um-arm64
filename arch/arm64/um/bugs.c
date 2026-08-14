// SPDX-License-Identifier: GPL-2.0
/*
 * Host-side sanity checks for ARCH=um SUBARCH=arm64.
 */
#include <arch.h>
#include <sysdep/ptrace.h>

void arch_check_bugs(void)
{
	/*
	 * Nothing to check. x86 historically probed for a broken SEP/sysenter
	 * host; aarch64 has a single, architecturally specified SVC path and no
	 * equivalent to detect.
	 */
}

void arch_examine_signal(int sig, struct uml_pt_regs *regs)
{
	/*
	 * x86 uses this to recognise a SIGTRAP that is really a hardware
	 * breakpoint. UML/arm64 does not implement hardware breakpoints or
	 * watchpoints for the guest, so every signal reaching here is what it
	 * appears to be.
	 */
}
