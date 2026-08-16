#!/bin/bash
# SPDX-License-Identifier: GPL-2.0
#
# faultcmp.sh -- compare guest and native memory costs at a MATCHED footprint.
#
# The comparison this replaces was wrong in two ways at once, and both ways
# flattered nobody -- they just made the number meaningless:
#
#   1. perfbench reports per PAGE and steps by sysconf(_SC_PAGESIZE), so a 16K
#      guest was being compared against a 4K host on a unit four times larger.
#      "11us/page vs 1.3us/page" was really 11us per 16K against 5.2us per 16K.
#   2. Guest and native binaries were built by different compilers against
#      different libcs, so even the parts with no kernel involvement differed.
#      The same source, built by clang 19/glibc and clang 18/bionic, differs by
#      2x on a plain store loop.
#
# So: ONE static binary, run on both sides, and page counts chosen per side to
# cover the same number of BYTES.
#
# Footprint matters more than it looks. This phone runs with a few hundred MB
# free, and a working set the host cannot keep resident turns the measurement
# into a measurement of host reclaim -- the guest sees no faults of its own
# (the host takes them), so nothing in the guest's own numbers says that is
# what happened. Keep MB modest and watch the spread: a stable native column
# beside a guest column that swings 4x is the tell.
#
# Usage: MB=32 REPS=5 harness/faultcmp.sh [serial]
set -uo pipefail

DEV=/data/local/tmp
SER=${1:-192.168.1.243:39685}
A="adb -s $SER"
MB=${MB:-32}
REPS=${REPS:-5}
MASK=${MASK:-30}
KERNEL=${KERNEL:-$DEV/k-test}
MEM=${MEM:-512M}
GUEST_PAGE=${GUEST_PAGE:-16384}
HOST_PAGE=${HOST_PAGE:-4096}

gpages=$((MB * 1024 * 1024 / GUEST_PAGE))
hpages=$((MB * 1024 * 1024 / HOST_PAGE))

# Free memory is part of the result, not decoration: a working set the host
# cannot keep resident turns this into a measurement of host reclaim, and the
# guest's own counters show nothing because the host takes those faults.
hostfree() {
	$A shell free -m 2>/dev/null | awk '/^Mem:/ { print $4 " MB" }' | tr -d '\r'
}

echo "### matched footprint ${MB} MB: guest ${gpages}x${GUEST_PAGE}, native ${hpages}x${HOST_PAGE}"
echo "### host free before: $(hostfree)"

native=$($A shell "cd $DEV && taskset $MASK ./clockcheck 2 >/dev/null 2>&1
for i in \$(seq $REPS); do taskset $MASK ./faultbench-glibc $hpages; done" 2>&1 | tr -d '\r')

guest=$($A shell "cd $DEV && HOME=$DEV taskset $MASK timeout 300 $KERNEL \
    mem=$MEM panic=-1 con=null con0=null,fd:1 \
    initrd=$DEV/initramfs-fb.cpio.gz init=/init seccomp=on \
    fbpages=$gpages fbreps=$REPS ${EXTRA:-}" 2>&1 | tr -d '\r')

# Median per phase, and the spread, because a median alone hides the reclaim
# problem this script exists to make visible.
summarise() {
	awk -v tag="$1" -v bytes="$((MB * 1024 * 1024))" '
	/us\/page/ {
		name = $1; mbs = $4
		v[name] = v[name] " " mbs; n[name]++
		if (!(name in seen)) { order[++k] = name; seen[name] = 1 }
	}
	END {
		for (i = 1; i <= k; i++) {
			nm = order[i]; c = split(v[nm], a, " ")
			for (x = 1; x < c; x++) for (y = x + 1; y <= c; y++)
				if (a[y] + 0 < a[x] + 0) { t = a[x]; a[x] = a[y]; a[y] = t }
			med = (c % 2) ? a[(c + 1) / 2] : (a[c / 2] + a[c / 2 + 1]) / 2
			printf "%-8s %-10s median %8.1f MB/s   min %8.1f  max %8.1f  (n=%d)\n",
			       tag, nm, med, a[1], a[c], c
		}
	}'
}

echo
echo "$native" | summarise native
echo
echo "$guest" | summarise guest
echo
echo "### host free after: $(hostfree)"
