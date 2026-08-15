// SPDX-License-Identifier: GPL-2.0-only
/*
 * Instruction patching for ARCH=um SUBARCH=arm64.
 *
 * arch/arm64/kernel/module.c is reused here to relocate modules, and it writes
 * the relocated instructions through aarch64_insn_copy(). On hardware that
 * function exists because kernel text is mapped read-only: it creates a
 * writable alias of the target page, copies through it, and then performs the
 * cache maintenance that makes the new instructions visible to the instruction
 * fetch path.
 *
 * A UML guest has no read-only kernel text to work around -- module memory is
 * ordinary writable memory in an ordinary process -- so the writable-alias half
 * of the problem does not exist and a plain memcpy is correct.
 *
 * The cache maintenance half very much does still exist, and is the reason this
 * is not simply a memcpy. These are real instructions being written to a real
 * arm64 CPU, which has separate instruction and data caches that are not
 * coherent with each other. Without cleaning the new bytes out of the D-cache
 * to the point of unification and invalidating the I-cache for that range, the
 * CPU is entitled to execute whatever was in the I-cache before -- which, for
 * freshly vmalloc'd module memory, is stale data or another module's code. The
 * failure would be intermittent, would depend on cache pressure, and would look
 * like a random crash inside a newly loaded module.
 *
 * __builtin___clear_cache() is the right primitive here rather than arm64's
 * caches_clean_inval_pou(): that one lives in arch/arm64/mm/cache.S, which is
 * not built for ARCH=um, and it is written for EL1. The compiler builtin emits
 * the EL0-legal sequence (DC CVAU / DSB ISH / IC IVAU / DSB ISH / ISB), which is
 * exactly what a userspace JIT does and exactly what this is.
 */
#include <linux/kernel.h>
#include <linux/string.h>
#include <linux/types.h>

#include <asm/text-patching.h>

void *aarch64_insn_copy(void *dst, void *src, size_t len)
{
	memcpy(dst, src, len);

	__builtin___clear_cache((char *)dst, (char *)dst + len);

	return dst;
}
