/* SPDX-License-Identifier: GPL-2.0 */
#ifndef __UM_ARM64_MMAN_H
#define __UM_ARM64_MMAN_H

/*
 * arch/arm64/include/asm/mman.h maps PROT_BTI, PROT_MTE and the guarded
 * control stack onto VM_ARM64_BTI / VM_MTE / VM_SHADOW_STACK, gated on
 * system_supports_bti() and friends -- all EL1 cpufeature state that does not
 * exist in UML.
 *
 * A UML guest cannot have any of these: BTI, MTE and GCS are enforced by the
 * MMU and the exception level, and a UML guest's memory protection is whatever
 * the host grants the stub process through plain mprotect(). Falling back to
 * the generic no-op hooks is therefore accurate, not a stub: the guest reports
 * these features as absent because they genuinely are.
 */
#include <uapi/asm/mman.h>

#endif /* __UM_ARM64_MMAN_H */
