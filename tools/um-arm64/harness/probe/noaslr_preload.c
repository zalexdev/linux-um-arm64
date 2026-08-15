// SPDX-License-Identifier: GPL-2.0
/*
 * Make personality(ADDR_NO_RANDOMIZE) fail, to simulate a host that forbids it.
 *
 * arch/um/os-Linux/main.c asks for ADDR_NO_RANDOMIZE and re-execs itself if the
 * request took effect. The goal states that personality() may be blocked on
 * Android, and the address-space layout that now works must not depend on it --
 * so this shim reproduces that host, locally, today, rather than discovering it
 * on a phone after the layout is settled.
 *
 * Build:
 *   clang --target=aarch64-linux-gnu -shared -fPIC -o noaslr_preload.so \
 *         noaslr_preload.c
 * Use:
 *   LD_PRELOAD=./noaslr_preload.so ./linux ...
 */
#define _GNU_SOURCE
#include <errno.h>

int personality(unsigned long persona);

int personality(unsigned long persona)
{
	(void)persona;
	errno = EPERM;
	return -1;
}
