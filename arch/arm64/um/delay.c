// SPDX-License-Identifier: GPL-2.0
/*
 * Busy-wait loops for ARCH=um SUBARCH=arm64.
 */
#include <linux/export.h>
#include <linux/kernel.h>
#include <linux/delay.h>
#include <asm/param.h>

/*
 * A calibrated spin. "yield" is a hint the core may ignore, so the loop is a
 * real dependent-subtract chain rather than relying on the hint for timing --
 * exactly like the aligned jump chain x86 uses, but arm64 needs no alignment
 * tricks because its instructions are fixed width and already aligned.
 */
void __delay(unsigned long loops)
{
	unsigned long tmp = loops;

	if (!tmp)
		return;

	__asm__ __volatile__(
		"1:	subs	%0, %0, #1\n"
		"	bne	1b\n"
		: "+r" (tmp)
		:
		: "cc");
}
EXPORT_SYMBOL(__delay);

/*
 * xloops is a 32.32 fixed-point count of jiffies-worth of loops. The x86
 * version does this with a 32x32->64 mull; on arm64 a plain 64-bit multiply and
 * shift is both shorter and clearer.
 */
inline void __const_udelay(unsigned long xloops)
{
	unsigned long loops;

	loops = ((unsigned long long)xloops * loops_per_jiffy * HZ) >> 32;

	__delay(++loops);
}
EXPORT_SYMBOL(__const_udelay);

void __udelay(unsigned long usecs)
{
	__const_udelay(usecs * 0x000010c7); /* 2**32 / 1000000 (rounded up) */
}
EXPORT_SYMBOL(__udelay);

void __ndelay(unsigned long nsecs)
{
	__const_udelay(nsecs * 0x00005); /* 2**32 / 1000000000 (rounded up) */
}
EXPORT_SYMBOL(__ndelay);
