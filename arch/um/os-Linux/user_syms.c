// SPDX-License-Identifier: GPL-2.0
#define __NO_FORTIFY
#include <linux/types.h>
#include <linux/module.h>

/*
 * This file exports some critical string functions and compiler
 * built-in functions (where calls are emitted by the compiler
 * itself that we cannot avoid even in kernel code) to modules.
 *
 * "_user.c" code that previously used exports here such as hostfs
 * really should be considered part of the 'hypervisor' and define
 * its own API boundary like hostfs does now; don't add exports to
 * this file for such cases.
 */

/* If it's not defined, the export is included in lib/string.c.*/
#ifdef __HAVE_ARCH_STRSTR
#undef strstr
EXPORT_SYMBOL(strstr);
#endif

/*
 * Who exports memcpy/memmove/memset differs per subarch, and exactly one of
 * them must:
 *
 *   i386   -- <asm/string_32.h> defines __HAVE_ARCH_MEMCPY, so lib/string.c
 *             does not build them, and nothing else exports the libc versions
 *             the compiler emits calls to. This file has to.
 *   x86_64 -- same __HAVE_ARCH_MEMCPY, but arch/x86/lib/memcpy_64.S and friends
 *             are linked in via subarch-y and carry their own EXPORT_SYMBOL.
 *   arm64  -- arch/arm64/um/asm/string.h deliberately defines no __HAVE_ARCH_*,
 *             because arch/arm64/lib is not built for ARCH=um, so lib/string.c
 *             supplies and exports the generic C versions.
 *
 * Exporting here as well makes modpost fail the build with "'memcpy' exported
 * twice", which is only visible once CONFIG_MODULES is on -- so it appeared the
 * moment arm64 gained module support, not when the subarch was written.
 */
#if !defined(__x86_64__) && !defined(__aarch64__)
#undef memcpy
extern void *memcpy(void *, const void *, size_t);
EXPORT_SYMBOL(memcpy);
extern void *memmove(void *, const void *, size_t);
EXPORT_SYMBOL(memmove);
#undef memset
extern void *memset(void *, int, size_t);
EXPORT_SYMBOL(memset);
#endif

#ifdef _FORTIFY_SOURCE
extern int __sprintf_chk(char *str, int flag, size_t len, const char *format);
EXPORT_SYMBOL(__sprintf_chk);
#endif
