/* SPDX-License-Identifier: GPL-2.0 */
#ifndef __UM_ARM64_STRING_H
#define __UM_ARM64_STRING_H

/*
 * arch/arm64/include/asm/string.h declares hand-written assembly versions of
 * strrchr, strchr, strcmp, memcpy and friends, implemented in arch/arm64/lib.
 * Those are kernel-only routines and arch/arm64/lib is not built for ARCH=um,
 * so declaring __HAVE_ARCH_* here would suppress lib/string.c's generic C
 * versions and leave the symbols undefined at link time.
 *
 * Defining no __HAVE_ARCH_* at all lets lib/string.c supply everything. That
 * also keeps UML's -Dstrrchr=kernel_strrchr renaming (see arch/um/Makefile)
 * working, which is what stops the kernel's string functions from colliding with
 * the libc that the UML binary is linked against.
 */


/*
 * Against bionic, let libc supply these.
 *
 * The comment above explains why no __HAVE_ARCH_* is declared for a glibc
 * build: arch/arm64/lib is not compiled for ARCH=um, so claiming the arch has
 * its own would leave the symbols undefined. Against bionic the situation is
 * the opposite -- libc.a certainly has them, and its objects get pulled in
 * regardless because bionic's libc is coarser-grained than glibc's, so the
 * kernel's copies in lib/string.c collide with libc's at static link time.
 *
 * Declaring them here stops lib/string.c compiling its own, which removes the
 * collision at the source rather than renaming one side of it. Renaming is the
 * wrong tool for this family in particular: CONFIG_FORTIFY_SOURCE redefines
 * these names as macros, so a -Dstrchr=kernel_strchr fights the fortify
 * wrappers and produces "call to undeclared library function 'strchr'".
 */
#ifdef __ANDROID__
#define __HAVE_ARCH_MEMCPY
#define __HAVE_ARCH_MEMSET
#define __HAVE_ARCH_MEMMOVE
#define __HAVE_ARCH_MEMCMP
#define __HAVE_ARCH_MEMCHR
#define __HAVE_ARCH_STRCMP
#define __HAVE_ARCH_STRCPY
#define __HAVE_ARCH_STRCHR
#define __HAVE_ARCH_STRCHRNUL
#define __HAVE_ARCH_STRLEN
#define __HAVE_ARCH_STRNCMP
#define __HAVE_ARCH_STRNLEN

/*
 * __HAVE_ARCH_* tells <linux/string.h> that the architecture declares these,
 * so it stops declaring them itself. Here "the architecture" is libc, so
 * declare them with the signatures libc uses and let the link resolve them
 * there.
 */
void *memcpy(void *, const void *, __kernel_size_t);
void *memset(void *, int, __kernel_size_t);
void *memmove(void *, const void *, __kernel_size_t);
int memcmp(const void *, const void *, __kernel_size_t);
void *memchr(const void *, int, __kernel_size_t);
int strcmp(const char *, const char *);
char *strcpy(char *, const char *);
char *strchr(const char *, int);
char *strchrnul(const char *, int);
__kernel_size_t strlen(const char *);
int strncmp(const char *, const char *, __kernel_size_t);
__kernel_size_t strnlen(const char *, __kernel_size_t);
#endif

#endif /* __UM_ARM64_STRING_H */
