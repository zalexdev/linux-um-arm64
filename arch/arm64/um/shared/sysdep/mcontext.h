/* SPDX-License-Identifier: GPL-2.0 */
#ifndef __SYS_SIGCONTEXT_ARM64_H
#define __SYS_SIGCONTEXT_ARM64_H

#include <stub-data.h>

extern void get_regs_from_mc(struct uml_pt_regs *, mcontext_t *);
extern void get_mc_from_regs(struct uml_pt_regs *regs, mcontext_t *mc,
			     int single_stepping);

extern int get_stub_state(struct uml_pt_regs *regs, struct stub_data *data,
			  unsigned long *fp_size_out);
extern int set_stub_state(struct uml_pt_regs *regs, struct stub_data *data,
			  int single_stepping);

/*
 * Records chained inside mcontext.__reserved[]. Spelled out rather than pulled
 * from <asm/sigcontext.h> because this header is included by stub_segv.c, which
 * is compiled freestanding for the stub and must not acquire new dependencies.
 */
#define UM_FPSIMD_MAGIC	0x46508001
#define UM_ESR_MAGIC	0x45535201
#define UM_EXTRA_MAGIC	0x45585401
#define UM_SVE_MAGIC	0x53564501

struct um_aarch64_ctx {
	unsigned int magic;
	unsigned int size;
};

/*
 * Walk the __reserved chain looking for the ESR record the kernel deposits for
 * SIGSEGV and SIGBUS. Returns 0 if it is absent, which is indistinguishable
 * from "a data read fault" for our purposes and is the safe default: treating an
 * unknown fault as a read merely costs an extra fault if it was really a write,
 * whereas guessing "write" would silently break COW.
 *
 * The walk is bounded by both the record sizes and the buffer length, because
 * this parses a structure the guest could in principle have influenced.
 */
static inline unsigned long um_mc_find_esr(mcontext_t *mc)
{
	unsigned char *p = (unsigned char *)mc->__reserved;
	unsigned long left = sizeof(mc->__reserved);

	while (left >= sizeof(struct um_aarch64_ctx)) {
		struct um_aarch64_ctx *h = (struct um_aarch64_ctx *)p;

		if (h->magic == 0 || h->size == 0)
			break;
		if (h->size > left)
			break;
		if (h->magic == UM_ESR_MAGIC) {
			if (h->size < sizeof(*h) + sizeof(unsigned long))
				break;
			return *(unsigned long *)(p + sizeof(*h));
		}
		p += h->size;
		left -= h->size;
	}
	return 0;
}

/*
 * arm64 puts the faulting address directly in mcontext_t, so unlike x86 there
 * is nothing to dig out of a gregs[] array. The write flag comes from ESR.WnR.
 */
#define GET_FAULTINFO_FROM_MC(fi, mc)					\
	do {								\
		unsigned long __esr = um_mc_find_esr(mc);		\
									\
		(fi).addr = (unsigned long)(mc)->fault_address;		\
		(fi).esr = (unsigned int)__esr;				\
		(fi).is_write = !!(__esr & UM_ESR_WNR) &&		\
			(UM_ESR_EC(__esr) == UM_ESR_EC_DABT_LOW ||	\
			 UM_ESR_EC(__esr) == UM_ESR_EC_DABT_CUR);	\
	} while (0)

#endif /* __SYS_SIGCONTEXT_ARM64_H */
