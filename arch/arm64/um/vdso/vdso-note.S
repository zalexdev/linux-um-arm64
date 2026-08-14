/* SPDX-License-Identifier: GPL-2.0 */
/*
 * A vDSO is a real shared object; give it the usual kernel version note so that
 * tools which inspect it (and glibc, which parses it out of AT_SYSINFO_EHDR)
 * see a well-formed DSO.
 */
#include <linux/build-salt.h>
#include <linux/uts.h>
#include <linux/version.h>
#include <linux/elfnote.h>

ELFNOTE_START(Linux, 0, "a")
	.long LINUX_VERSION_CODE
ELFNOTE_END

BUILD_SALT
