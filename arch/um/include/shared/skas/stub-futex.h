/* SPDX-License-Identifier: GPL-2.0 */
/*
 * Shared halves of the stub <-> kernel handoff: waiter-bit wake elision and
 * the bounded adaptive spin. Everything here is __always_inline because the
 * stub side must end up inside .__syscall_stub with no calls out of it.
 *
 * Used from both stub_signal_interrupt() (the stub) and
 * wait_stub_done_seccomp() (the UML kernel thread). The parking step itself
 * (fetch_or of the waiter bit + FUTEX_WAIT loop) stays in the callers: the
 * two sides make the futex syscall differently and handle its failure
 * differently, and hiding that behind one helper obscured more than it saved.
 *
 * MEMORY ORDERING. The futex syscalls that used to bracket every handoff are
 * full barriers, and the whole point of this file is to skip those syscalls;
 * the ordering must therefore come from the accesses themselves. Every write
 * that flips ownership is a release (stub_futex_xchg), every read that
 * decides "the peer is done" is an acquire (stub_futex_load_acquire), so the
 * data written before the flip -- syscall_data, signal, mctx offsets -- is
 * visible to whoever observes the flip. This is an arm64-only stale-data
 * class: x86-TSO gives these guarantees to plain accesses, so no amount of
 * x86 testing can catch getting it wrong here. See the sysdep stub.h of each
 * arch for the primitives.
 */
#ifndef __STUB_FUTEX_H
#define __STUB_FUTEX_H

#include <sysdep/stub.h>

/* Streak cap only guards against wraparound; any small value works. */
#define STUB_SPIN_STREAK_MAX	16
/* Waits between spin probes while the spin keeps missing. */
#define STUB_SPIN_BACKOFF	16

/*
 * Hand ownership of the protocol word to the peer, publishing (release) every
 * write made before the call. Returns nonzero when the peer parked in
 * FUTEX_WAIT and a FUTEX_WAKE is actually needed; a peer caught spinning (or
 * still running) observes the exchange on its own.
 */
static __always_inline int
stub_futex_hand_over(volatile unsigned int *futex, unsigned int to)
{
	return stub_futex_xchg(futex, to) & STUB_FUTEX_WAITER;
}

/*
 * How long this wait may spin, in stub_cycles() ticks. Zero means "go park".
 */
static __always_inline unsigned long
stub_futex_spin_budget(struct stub_spin_state *s, unsigned int spin_ticks)
{
	if (!spin_ticks)
		return 0;

	if (s->streak)
		return spin_ticks;

	/* Recent miss: sleep for a while before probing the spin again. */
	if (s->backoff) {
		s->backoff--;
		return 0;
	}

	return spin_ticks;
}

/* Record the outcome of an actually-attempted spin. */
static __always_inline void
stub_futex_spin_result(struct stub_spin_state *s, int hit)
{
	if (hit) {
		if (s->streak < STUB_SPIN_STREAK_MAX)
			s->streak++;
	} else {
		s->streak = 0;
		s->backoff = STUB_SPIN_BACKOFF;
	}
}

/*
 * Poll the word until ownership leaves `while_owner`, for at most `ticks`
 * ticks of wall time. Returns the last value observed, read with acquire
 * semantics, so on a hit the caller may immediately read the peer's data.
 *
 * The bound is wall time by construction, never an iteration count: the
 * budget must mean the same thing at the governor's 300 MHz floor and its
 * 3.2 GHz ceiling, and only the constant-rate counter delivers that.
 */
static __always_inline unsigned int
stub_futex_spin(volatile unsigned int *futex, unsigned int while_owner,
		unsigned long ticks)
{
	unsigned int val = stub_futex_load_acquire(futex);
	unsigned long start;

	if (STUB_FUTEX_OWNER(val) != while_owner || !ticks)
		return val;

	start = stub_cycles();
	do {
		stub_relax();
		val = stub_futex_load_acquire(futex);
		if (STUB_FUTEX_OWNER(val) != while_owner)
			break;
	} while (stub_cycles() - start < ticks);

	return val;
}

#endif /* __STUB_FUTEX_H */
