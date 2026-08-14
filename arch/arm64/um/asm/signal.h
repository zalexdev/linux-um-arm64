/* SPDX-License-Identifier: GPL-2.0 */
#ifndef __UM_ARM64_SIGNAL_H
#define __UM_ARM64_SIGNAL_H

/*
 * arch/arm64/include/asm/signal.h exists solely to define
 * arch_untagged_si_addr(), which strips address tags from si_addr for TBI and
 * MTE. It reaches that through <asm/memory.h>, i.e. the whole EL1 virtual
 * memory layout.
 *
 * A UML guest has neither TBI nor MTE: the guest's "hardware" is the host
 * process, UML does not implement tagged pointers, and si_addr values reaching
 * the guest have already been untagged (if at all) by the host. So there is
 * nothing to strip, and the generic behaviour of leaving si_addr alone is
 * correct.
 */
#include <uapi/asm/signal.h>
#include <uapi/asm/siginfo.h>

#endif /* __UM_ARM64_SIGNAL_H */
