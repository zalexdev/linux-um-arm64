/* SPDX-License-Identifier: GPL-2.0 */
#ifndef __UM_ARM64_CHECKSUM_H
#define __UM_ARM64_CHECKSUM_H

/*
 * arm64 has hand-written checksum helpers in arch/arm64/lib, but they are
 * written against the kernel's uaccess model. UML's is different, and the
 * generic C versions are correct and fast enough for a guest whose network
 * path is a host socket anyway.
 */
#include <asm-generic/checksum.h>

#endif
