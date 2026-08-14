// SPDX-License-Identifier: GPL-2.0
/*
 * SIGSEGV handler that runs *inside* the stub process, with no libc and no
 * stack protector. It records where and why the fault happened into the stub's
 * data page and then traps, handing control back to the UML kernel.
 */
#include <sysdep/stub.h>
#include <sysdep/faultinfo.h>
#include <sysdep/mcontext.h>
#include <sys/ucontext.h>

void __attribute__ ((__section__ (".__syscall_stub")))
stub_segv_handler(int sig, siginfo_t *info, void *p)
{
	struct faultinfo *f = get_stub_data();
	ucontext_t *uc = p;

	GET_FAULTINFO_FROM_MC(*f, &uc->uc_mcontext);
	trap_myself();
}
