/* SPDX-License-Identifier: GPL-2.0 */
#ifndef __ARCH_STUB_DATA_H
#define __ARCH_STUB_DATA_H

/*
 * Guest TLS on arm64 is TPIDR_EL0, which userspace owns outright: the guest
 * sets it with "msr tpidr_el0, xN" and never asks the kernel. There is no
 * set_thread_area/arch_prctl equivalent to intercept, so the only thing UML has
 * to do is reinstate the register when it resumes a different guest thread in
 * the same stub process.
 */
#define STUB_SYNC_TLS (1 << 0)

struct stub_data_arch {
	int sync;
	unsigned long tls;
};

#endif /* __ARCH_STUB_DATA_H */
