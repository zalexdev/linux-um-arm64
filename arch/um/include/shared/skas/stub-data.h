/* SPDX-License-Identifier: GPL-2.0 */
/*

 * Copyright (C) 2015 Thomas Meyer (thomas@m3y3r.de)
 * Copyright (C) 2005 Jeff Dike (jdike@karaya.com)
 */

#ifndef __STUB_DATA_H
#define __STUB_DATA_H

#include <linux/compiler_types.h>
#include <as-layout.h>
#include <sysdep/tls.h>
#include <sysdep/stub-data.h>
#include <mm_id.h>

/* See the note in skas/mm_id.h: a sysroot's linux/compiler_types.h can shadow
 * the kernel's, leaving the attribute macros undefined for USER_CFLAGS objects.
 */
#ifndef __aligned
#define __aligned(x)	__attribute__((__aligned__(x)))
#endif
#ifndef __section
#define __section(x)	__attribute__((__section__(x)))
#endif
#ifndef __used
#define __used		__attribute__((__used__))
#endif

/*
 * The stub <-> kernel handoff word. The low bit names the side that currently
 * owns the CPU ("the child may run" / "the kernel may run"); ownership strictly
 * ping-pongs, so at any moment at most one side is waiting.
 */
#define FUTEX_IN_CHILD 0
#define FUTEX_IN_KERN 1
/*
 * ORed in by a waiter that is about to park in FUTEX_WAIT. A waker hands
 * ownership over with an atomic exchange and calls FUTEX_WAKE only if the old
 * value carried this bit: on this host a FUTEX_WAKE that actually has someone
 * to wake costs a voluntary context switch (the measured 37k voluntary
 * switches per benchmark run are exactly these), and even an empty one is a
 * syscall. When the peer was caught mid-spin, the wake is pure waste.
 *
 * The bit can go stale in one benign way: the waiter sets it just after the
 * waker's exchange already flipped ownership. The waiter then sees the flip in
 * the fetch_or's old value and never parks, but the bit stays set until the
 * next exchange clears it, costing that exchange one spurious FUTEX_WAKE.
 * Rare and harmless; a scheme that cleans the bit up would need a second
 * atomic on every wait, which is the common path.
 */
#define STUB_FUTEX_WAITER 2

/* The ownership half of the protocol word, waiter bit masked off. */
#define STUB_FUTEX_OWNER(v) ((v) & FUTEX_IN_KERN)

/*
 * 64 covers the cacheline size of every x86-64 and arm64 host this can run on
 * (Snapdragon's Cortex/Kryo cores included).
 */
#define STUB_FUTEX_ALIGN 64

/*
 * Adaptive-spin bookkeeping, one instance per side, each written only by its
 * owner (no atomics needed):
 *
 *  - streak: consecutive spins that observed the reply within the budget.
 *    While nonzero, keep spinning: the peer is answering fast.
 *  - backoff: after a miss, the number of upcoming waits that go straight to
 *    FUTEX_WAIT before the spin is probed again. Without this an idle or
 *    compute-heavy guest would burn the full budget on every single wait; with
 *    it the steady-state cost of a never-hitting spin is one budget per
 *    STUB_SPIN_BACKOFF waits, while a workload that turns syscall-heavy again
 *    is picked back up within that many waits.
 */
struct stub_spin_state {
	unsigned short streak;
	unsigned short backoff;
};

struct stub_init_data {
	int seccomp;

	unsigned long stub_start;

	int stub_code_fd;
	unsigned long stub_code_offset;
	int stub_data_fd;
	unsigned long stub_data_offset;

	unsigned long signal_handler;
	unsigned long signal_restorer;
};

#define STUB_NEXT_SYSCALL(s) \
	((struct stub_syscall *) (((unsigned long) s) + (s)->cmd_len))

enum stub_syscall_type {
	STUB_SYSCALL_UNSET = 0,
	STUB_SYSCALL_MMAP,
	STUB_SYSCALL_MUNMAP,
};

struct stub_syscall {
	struct {
		unsigned long addr;
		unsigned long length;
		unsigned long offset;
		int fd;
		int prot;
	} mem;

	enum stub_syscall_type syscall;
};

struct stub_data {
	long err;

	int syscall_data_len;

	/*
	 * The handoff word, alone in a 64-byte slot. Both sides poll it now
	 * (see stub-futex.h); if it shared a cacheline with anything the peer
	 * writes -- syscall_data, signal, the spin state -- each such write
	 * would yank the line from the polling CPU and the spin would spend
	 * its budget on coherence misses instead of observing the flip. The
	 * explicit pad keeps the tail of the line empty; init_child_tracking()
	 * BUILD_BUG_ONs the isolation so a reorder here cannot silently undo
	 * it.
	 */
	unsigned int futex __aligned(STUB_FUTEX_ALIGN);
	unsigned char futex_pad[STUB_FUTEX_ALIGN - sizeof(unsigned int)];

	/*
	 * 256 leaves enough room for the fields around the array: 128 bytes
	 * above, the rest below. The static_assert on sizeof(struct stub_data)
	 * in mmu.c catches this reserve becoming too small on any page size
	 * or subarch, because the tail would then push the page-aligned
	 * sigstack into an extra page.
	 */
	struct stub_syscall syscall_data[(UM_KERN_PAGE_SIZE - 256) / sizeof(struct stub_syscall)] __aligned(16);

	/* data shared with signal handler (only used in seccomp mode) */
	short restart_wait;
	int signal;
	unsigned short si_offset;
	unsigned short mctx_offset;

	/*
	 * Spin budget in ticks of the timebase behind stub_cycles(), written
	 * once by the kernel side before the stub first runs; 0 means "park
	 * immediately" and is what seccomp_spin=0 and hosts without a usable
	 * constant-rate EL0 counter (x86) get. Ticks of wall time rather than
	 * an iteration count is deliberate: this class of SoC scales
	 * 300 MHz - 3.2 GHz, so a loop count would mean a 10x different
	 * duration depending on the governor's mood, while CNTVCT_EL0 runs at
	 * a constant rate regardless.
	 */
	unsigned int spin_ticks;
	struct stub_spin_state kern_spin;	/* written by the kernel side only */
	struct stub_spin_state stub_spin;	/* written by the stub only */

	/* seccomp architecture specific state restore */
	struct stub_data_arch arch_data;

	/*
	 * Stack for our signal handlers and for calling into .
	 *
	 * Sized from STUB_DATA_PAGES rather than fixed at one page: in SECCOMP
	 * mode the host writes a whole signal frame here, and on arm64 that
	 * frame is larger than a 4 KiB page. See the comment on STUB_DATA_PAGES
	 * in as-layout.h.
	 */
	unsigned char sigstack[(STUB_DATA_PAGES - 1) * UM_KERN_PAGE_SIZE]
		__aligned(UM_KERN_PAGE_SIZE);
};

#endif
