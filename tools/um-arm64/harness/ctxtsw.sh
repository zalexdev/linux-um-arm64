#!/bin/bash
# How many host context switches does one guest syscall cost?
#
# The interesting number is not the wall clock but the mechanism behind it. In
# SECCOMP mode a guest syscall is: SIGSYS delivered to the stub, the stub writes
# the request into the shared page and futex-wakes the UML kernel thread, the
# kernel thread services it and futex-wakes the stub back. That is at least two
# host thread switches per guest syscall, and on a big.LITTLE phone a switch
# that crosses clusters costs far more than one that does not.
#
# So count them. The benchmark issues a known number of guest syscalls, so
# dividing gives switches-per-syscall directly, and the voluntary vs
# nonvoluntary split says whether the threads are handing off to each other
# (voluntary: futex waits) or being preempted by the scheduler (nonvoluntary:
# the scheduler getting in the way, which is what pinning is meant to fix).
#
# Counted across the UML process and every stub child, since the stub is a
# separate process rather than a thread.
set -uo pipefail

DEV=/data/local/tmp
MASK=${MASK:-70}
MODE=${MODE:-seccomp=on}

adb shell "
cd $DEV || exit 1
HOME=$DEV TMPDIR=/tmp taskset $MASK timeout 300 ./linux-sec mem=512M panic=-1 \
    con=null con0=null,fd:1 initrd=initramfs-perf.cpio.gz init=/init $MODE \
    > cs.out 2>&1 &
umlpid=\$!

# Poll while it lives, keeping the last successful reading. The process tree
# disappears at exit, so a snapshot taken afterwards would find nothing.
last=''
while kill -0 \$umlpid 2>/dev/null; do
    tot_v=0; tot_n=0; procs=0
    for p in \$(pgrep -f 'linux-sec|uml-userspace' 2>/dev/null); do
        for st in /proc/\$p/task/*/status; do
            [ -r \"\$st\" ] || continue
            v=\$(grep -a '^voluntary_ctxt_switches' \"\$st\" 2>/dev/null | awk '{print \$2}')
            n=\$(grep -a '^nonvoluntary_ctxt_switches' \"\$st\" 2>/dev/null | awk '{print \$2}')
            [ -n \"\$v\" ] && tot_v=\$((tot_v + v))
            [ -n \"\$n\" ] && tot_n=\$((tot_n + n))
            procs=\$((procs + 1))
        done
    done
    [ \"\$tot_v\" != 0 ] && last=\"\$tot_v \$tot_n \$procs\"
    sleep 0.3
done

wait \$umlpid 2>/dev/null
echo \"CTXTSW \$last\"
grep -aE '^syscall|^openat|^compute|Userspace mode' cs.out
"
