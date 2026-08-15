#!/bin/bash
# Boot ./linux in the aarch64 run domain and decide, mechanically, whether it worked.
#
# Design constraints from the goal, and why each exists:
#
#   * time-bounded  -- a failing port hangs far more often than it crashes, so
#                      every run is under `timeout` and every gate has a budget;
#   * marker string -- success is a string grepped out of the boot log, never an
#                      exit code, because UML can exit 0 having done nothing;
#   * panic=-1      -- so a guest panic terminates the process instead of sitting
#                      at a dead prompt until the timeout;
#   * gdb on SIGSEGV -- `linux` is an ordinary process, so a batch-mode backtrace
#                      is available and is the difference between a real signal
#                      and "does not boot";
#   * syscall log   -- a deliverable in its own right: what this binary asks of
#                      its host is the thing the Android phase has to fit inside.
#
# Everything lands under artifacts/<gate>/<stamp>/ on the *host*, via the 9p share.
set -uo pipefail

HERE=$(cd "$(dirname "$0")" && pwd)
ROOT=${ROOT:-/root/mlu-arm64}
VM="$HERE/vm.sh"

GATE=${GATE:-gate}
BIN=${BIN:-$ROOT/artifacts/linux-latest}
TIMEOUT=${TIMEOUT:-300}
MARKER=${MARKER:-UMARM_BOOT_OK}
MEM=${MEM:-512M}
EXTRA_ARGS=${EXTRA_ARGS:-}
INITRD=${INITRD:-}
UBD0=${UBD0:-}
INIT=${INIT:-/bin/sh}
PRELOAD=${PRELOAD:-}
STRACE=${STRACE:-0}   # strace cannot coexist with UML PTRACE_TRACEME; see doc/30-syscall-budget.md

# There is exactly one run domain, and every run stages its images into one
# fixed directory inside it. Two boot.sh invocations therefore cannot coexist:
# the second one's staging replaces the first one's root filesystem underneath a
# kernel that is already booting. That produced a gate-7 run which panicked with
# "VFS: Unable to mount root fs on unknown-block(0,0)" while an unrelated flake
# loop restaged an Alpine image over the Debian one -- a failure with no kernel
# in it at all, and precisely the sort of invented result an unattended loop must
# never be able to produce. Serialise instead of trusting the operator to
# remember what else is running.
exec 9>"${TMPDIR:-/tmp}/umarm-rundomain.lock"
if ! flock -w "${LOCK_WAIT:-7200}" 9; then
    echo "another run holds the run domain; giving up" >&2
    exit 2
fi

STAMP=$(date -u +%Y%m%dT%H%M%SZ)
OUT="$ROOT/artifacts/$GATE/$STAMP"
mkdir -p "$OUT"

log() { printf '[boot] %s\n' "$*"; }

# --- inputs, archived up front so a run is reproducible from its directory ---
cp -f "$BIN" "$OUT/linux" 2>/dev/null || { echo "no kernel at $BIN" >&2; exit 2; }
cp -f /tmp/umarm-build/.config "$OUT/config" 2>/dev/null
cp -f "$ROOT/artifacts/build-latest.log" "$OUT/build.log" 2>/dev/null
"$HERE/env-check.sh" > "$OUT/env.txt" 2>&1
UMARM_TREE="$ROOT/linux" "$HERE/env-check.sh" >> "$OUT/env.txt" 2>&1

CMDLINE="mem=$MEM panic=-1 init=$INIT con=null con0=fd:0,fd:1 $EXTRA_ARGS"
[ -n "$INITRD" ] && CMDLINE="initrd=/var/tmp/umboot/initrd $CMDLINE"
[ -n "$UBD0" ]   && CMDLINE="ubd0=/var/tmp/umboot/ubd0 root=/dev/ubda $CMDLINE"
echo "$CMDLINE" > "$OUT/cmdline"

# --- stage into the VM ------------------------------------------------------
# Old cores are large and only the newest is ever used; clearing them keeps a
# long unattended run from filling the run domain's disk.
# Truncate the previous run's log during staging, not just before the run: if
# staging fails and we bail out, the collect step must not pick up an older
# run's output and present it as this run's result.
# Kill anything left over from a previous run *before* touching the staging
# area. A UML that outlived its timeout still holds an F_SETLK on the ubd image
# and still has boot.log open, so the next run cannot open its root device and
# reads back the zombie's output as its own -- which presents as a kernel panic
# in a kernel that was never even started. Leftover state is how an unattended
# loop starts inventing failures.
$VM ssh "pkill -9 -x linux; pkill -9 -x uml-userspace; pkill -9 -f 'timeout .* ./linux'; sleep 1; true" >/dev/null 2>&1
$VM ssh "mkdir -p /var/tmp/umboot && rm -f /var/cores/core.* /var/tmp/umboot/boot.log /var/tmp/umboot/strace.log" || exit 2

# Copy only when the content changed. Staging is ~460MB; doing it unconditionally
# would dominate the wall clock of a repeat-until-flaky loop and make the loop
# too slow to actually run.
stage() {
    local src=$1 dst=$2 sum sz remote
    [ -n "$src" ] || return 0
    sum=$(md5sum "$src" | cut -d' ' -f1)
    sz=$(stat -c %s "$src")
    # The sidecar records the *source* checksum, not the destination's: a guest
    # that mounts its root device rw dirties the copy immediately, so comparing
    # the destination's own content would re-copy 2.7GB every single run.
    #
    # The size is checked separately and against the destination, because the
    # sidecar cannot detect a short transfer -- it is written by the same script
    # that did the copying, so a truncated image gets a confident checksum
    # describing a file that was never fully written. That produced a run which
    # panicked with "Unable to mount root fs on unknown-block(0,0)" and looked
    # exactly like a kernel bug.
    remote=$($VM ssh "stat -c %s /var/tmp/umboot/$dst 2>/dev/null; cat /var/tmp/umboot/$dst.md5 2>/dev/null" | tr '\n' ' ')
    if [ "$remote" = "$sz $sum " ]; then
        return 0
    fi
    # Copy to a side name and rename into place, rather than writing the
    # destination directly. `linux` is an executable that was running moments
    # ago, and open(O_WRONLY) on a live text image fails with ETXTBSY -- so a
    # run whose predecessor was still exiting would abort during staging with a
    # bare "scp: dest open ... Failure". rename(2) has no such restriction: the
    # old inode simply stays alive until the last user drops it. This also makes
    # staging atomic, so an interrupted copy cannot leave a half-written kernel
    # that the next run would happily try to boot.
    $VM scp "$src" "/var/tmp/umboot/$dst.new" >/dev/null || return 1
    # Verify before publishing. scp has been seen to exit 0 on a transfer that
    # did not deliver every byte, and an unattended loop must not be able to
    # boot an image it only partly copied.
    if [ "$($VM ssh "stat -c %s /var/tmp/umboot/$dst.new")" != "$sz" ]; then
        echo "stage: short transfer of $src -> $dst" >&2
        return 1
    fi
    $VM ssh "mv -f /var/tmp/umboot/$dst.new /var/tmp/umboot/$dst &&
             echo $sum > /var/tmp/umboot/$dst.md5" || return 1
}
stage "$OUT/linux" linux      || exit 2
stage "$INITRD"    initrd     || exit 2
stage "$UBD0"      ubd0       || exit 2
stage "$PRELOAD"   preload.so || exit 2

# A host directory for hostfs to mount. Always present, so a gate that wants it
# has something with known content to read, and the host side can be inspected
# afterwards to confirm the guest's writes really landed on the host.
#
# $HOSTSHARE_SRC makes the share a declared *input* of the run rather than
# whatever the last gate happened to leave lying there. It is worth the extra
# rsync: gate 7 once ran to completion reporting UMARM_NO_GATE_SCRIPT because an
# earlier hostfs test had cleaned the directory out, and a gate that silently
# tests nothing is worse than one that fails.
$VM ssh "mkdir -p /var/tmp/umboot/hostshare &&
         echo 'hello from the host, written before boot' > /var/tmp/umboot/hostshare/from-host.txt"
#
# Transferred with tar over ssh rather than rsync: the run domain is a stock
# Debian arm64 guest with no network and no rsync, and installing one to move
# test data would be exactly the kind of host requirement this port is supposed
# not to have. A content hash short-circuits the transfer, because the apt repo
# is ~112MB and re-sending it every iteration would dominate the loop.
if [ -n "${HOSTSHARE_SRC:-}" ]; then
    hs_sum=$(for d in $HOSTSHARE_SRC; do
                 find "$d" -type f -printf '%p %s %T@\n' 2>/dev/null | sort
             done | md5sum | cut -d' ' -f1)
    if [ "$($VM ssh 'cat /var/tmp/umboot/hostshare.md5 2>/dev/null')" != "$hs_sum" ]; then
        log "staging hostshare ($hs_sum)"
        $VM ssh "rm -f /var/tmp/umboot/hostshare.md5" >/dev/null || exit 2
        for d in $HOSTSHARE_SRC; do
            tar -C "$(dirname "$d")" -cf - "$(basename "$d")" |
                $VM ssh "tar -C /var/tmp/umboot/hostshare -xf -" || exit 2
        done
        $VM ssh "chmod -R a+rX /var/tmp/umboot/hostshare &&
                 echo $hs_sum > /var/tmp/umboot/hostshare.md5" >/dev/null || exit 2
    fi
fi

# --- run --------------------------------------------------------------------
# `setsid` + a process group kill keeps stub children from outliving a timeout.
# The exit status of `timeout` is preserved separately from the pipeline.
log "running gate=$GATE timeout=${TIMEOUT}s marker='$MARKER'"
$VM ssh "
set -u
cd /var/tmp/umboot
chmod +x linux
ulimit -c unlimited
rm -f boot.log strace.log core.*
STRACE_PREFIX=''
if [ '$STRACE' = 1 ] && command -v strace >/dev/null 2>&1; then
    STRACE_PREFIX='strace -f -qq -o strace.log -s 200'
fi
PRELOAD_ENV=''
if [ -n '$PRELOAD' ]; then PRELOAD_ENV='LD_PRELOAD=/var/tmp/umboot/preload.so'; fi
setsid env \$PRELOAD_ENV timeout --signal=KILL --kill-after=10 $TIMEOUT \
    \$STRACE_PREFIX ./linux $CMDLINE > boot.log 2>&1 </dev/null
echo \"RC=\$?\" >> boot.log
" >"$OUT/ssh.log" 2>&1

# --- collect ----------------------------------------------------------------
$VM ssh 'cat /var/tmp/umboot/boot.log 2>/dev/null'   > "$OUT/boot.log" 2>/dev/null
$VM ssh 'cat /var/tmp/umboot/strace.log 2>/dev/null' > "$OUT/strace.log" 2>/dev/null
$VM ssh 'ls -la /var/cores/ 2>/dev/null'       > "$OUT/cores.txt" 2>/dev/null
# What the guest left behind on the host, for the hostfs gate.
$VM ssh 'ls -la /var/tmp/umboot/hostshare/ 2>/dev/null; echo ---;
         cat /var/tmp/umboot/hostshare/from-guest.txt 2>/dev/null' > "$OUT/hostshare.txt" 2>/dev/null

RC=$(sed -n 's/^RC=//p' "$OUT/boot.log" | tail -1)
RC=${RC:-timeout}

# --- kernel diagnostics, scanned independently of the marker -----------------
# The marker rule ("success is a string in the log, never an exit code") is
# right about exit codes and silent about the log's other contents -- and the
# verdict chain below is an if/elif, so a run that printed its marker never
# reached the BUG: test at all. A 16 KB gate-6 run therefore recorded
# verdict=PASS with four "BUG: Bad rss-counter state" lines in its own boot log,
# and stayed green until someone read the log by hand. A gate that reaches its
# marker while the kernel reports a defect has not passed.
#
# Deliberately not matching "Kernel panic": several gates end by design in
# "Attempted to kill init!", which panics under panic=-1 *after* the marker.
# That is an expected end state, not a defect. Signatures here are ones the
# kernel only emits when something is actually wrong.
grep -aE '^(BUG:|WARNING:|Oops|INFO: rcu|INFO: task .* blocked|Unable to handle kernel|kernel BUG at)' \
     "$OUT/boot.log" 2>/dev/null | tr -d '\r' | sort | uniq -c | sort -rn > "$OUT/kernel-bugs.txt"
KBUGS=$(awk '{n += $1} END {print n+0}' "$OUT/kernel-bugs.txt")

# --- verdict ----------------------------------------------------------------
# Marker first: an exit code of 0 proves nothing here, and a non-zero one is
# expected for several gates (panic=-1 exits non-zero by design).
if grep -qF "$MARKER" "$OUT/boot.log" 2>/dev/null; then
    if [ "$KBUGS" -gt 0 ]; then
        VERDICT=BUG
    else
        VERDICT=PASS
    fi
elif [ "$RC" = "137" ] || [ "$RC" = "timeout" ] || [ "$RC" = "124" ]; then
    VERDICT=HANG
elif grep -qE "Kernel panic|BUG:|Oops" "$OUT/boot.log" 2>/dev/null; then
    VERDICT=PANIC
else
    VERDICT=FAIL
fi

# --- on a fatal signal, get a real backtrace --------------------------------
# 132=SIGILL 133=SIGTRAP 134=SIGABRT 135=SIGBUS 136=SIGFPE 139=SIGSEGV
case "$RC" in
132|133|134|135|136|139|11)
    # The marker wins. A gate whose init deliberately exits ends in
    # "Attempted to kill init!" and, with panic=-1, in a fatal signal -- which is
    # both the expected end state and indistinguishable at the process level from
    # a real crash. The goal is explicit that success is the marker string and
    # never an exit code alone, so only downgrade a run that did not reach its
    # marker. The backtrace is still captured either way: a crash *after* the
    # marker is worth having on file.
    case "$VERDICT" in PASS|BUG) ;; *) VERDICT=SIGNAL ;; esac
    log "fatal signal (rc=$RC), capturing backtrace"
    $VM ssh "
cd /var/tmp/umboot
CORE=\$(ls -t /var/cores/core.* 2>/dev/null | head -1)
if [ -n \"\$CORE\" ]; then
    gdb -batch -nx \
        -ex 'set pagination off' \
        -ex 'thread apply all bt full' \
        -ex 'info registers' \
        -ex 'info sharedlibrary' \
        ./linux \"\$CORE\" 2>&1
else
    echo 'no core file; re-running under gdb'
    gdb -batch -nx \
        -ex 'set pagination off' \
        -ex 'set follow-fork-mode parent' \
        -ex 'run $CMDLINE' \
        -ex 'thread apply all bt full' \
        -ex 'info registers' \
        --args ./linux $CMDLINE 2>&1 | tail -200
fi
" > "$OUT/backtrace.txt" 2>&1
    ;;
esac

# --- summary ----------------------------------------------------------------
{
    echo "gate=$GATE"
    echo "stamp=$STAMP"
    echo "verdict=$VERDICT"
    echo "rc=$RC"
    echo "marker=$MARKER"
    echo "cmdline=$CMDLINE"
    echo "preload=${PRELOAD:-none}"
    echo "boot_log_lines=$(wc -l < "$OUT/boot.log" 2>/dev/null || echo 0)"
    echo "syscalls_logged=$(wc -l < "$OUT/strace.log" 2>/dev/null || echo 0)"
    echo "kernel_bugs=$KBUGS"
    # Identify the run by what it *ran*. tree_head is `git rev-parse HEAD` at
    # capture time, which says nothing about which tree produced $BIN -- a run
    # can carry a HEAD five commits newer than the binary it booted, and one
    # already did. The md5 is the only field that names the artifact under test.
    echo "bin_md5=$(md5sum "$OUT/linux" 2>/dev/null | cut -d' ' -f1)"
    echo "tree_head=$(git -C "$ROOT/linux" rev-parse --short HEAD 2>/dev/null)"
} > "$OUT/result.txt"

if [ "$KBUGS" -gt 0 ]; then
    log "kernel reported $KBUGS diagnostic line(s):"
    sed 's/^/[boot]   /' "$OUT/kernel-bugs.txt"
fi

ln -sfn "$OUT" "$ROOT/artifacts/$GATE-latest"
log "verdict=$VERDICT rc=$RC -> $OUT"
cat "$OUT/result.txt"

[ "$VERDICT" = PASS ]
