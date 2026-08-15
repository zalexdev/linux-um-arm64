#!/bin/bash
# Run a gate on a real Android phone over adb, with the same discipline as
# harness/boot.sh: time-bounded, success is a marker string in the log and never
# an exit code, the log is scanned for kernel diagnostics independently of the
# marker, and everything lands in artifacts/ on this host.
#
# The phone is not just another host. Three things differ, and all three are
# properties of Android rather than of this port:
#
#   * HOME is unset under adb, so UML's umid directory becomes //.uml/ on a
#     read-only filesystem. It has to be set explicitly.
#   * TMPDIR is /data/local/tmp, which is f2fs. UML prefers TMPDIR over /tmp for
#     guest RAM, so without an override the guest's memory is backed by flash
#     rather than by the tmpfs that is sitting right there on /tmp.
#   * Everything runs as uid 2000 (shell) under enforcing SELinux. No root, no
#     new mounts, no kernel modules -- which is the environment the whole
#     syscall budget exists to fit inside.
#
# Usage: GATE=g3 INIT=/gate3-init UBD0=alpine.ext4 harness/android.sh
set -uo pipefail

ROOT=${ROOT:-/root/mlu-arm64}
DEV=/data/local/tmp
GATE=${GATE:-android}
BIN=${BIN:-$ROOT/artifacts/linux-android-4k}
MEM=${MEM:-512M}
TIMEOUT=${TIMEOUT:-300}
MARKER=${MARKER:-UMARM_BOOT_OK}
INIT=${INIT:-/init}
INITRD=${INITRD:-}
UBD0=${UBD0:-}
EXTRA_ARGS=${EXTRA_ARGS:-}
PUSH=${PUSH:-0}		# 1 to re-push the kernel; it is 77 MB over wifi

STAMP=$(date -u +%Y%m%dT%H%M%SZ)
OUT="$ROOT/artifacts/$GATE/$STAMP"
mkdir -p "$OUT"

log() { printf '[android] %s\n' "$*"; }

adb shell 'id; getprop ro.build.version.release; uname -r; getconf PAGESIZE' \
	2>/dev/null | tr -d '\r' > "$OUT/device.txt"

if [ "$PUSH" = 1 ]; then
	log "pushing $(basename "$BIN")"
	adb push "$BIN" "$DEV/linux-um" >/dev/null || exit 2
fi
adb shell "chmod 755 $DEV/linux-um" >/dev/null 2>&1
cp -f "$BIN" "$OUT/linux" 2>/dev/null

# con0=null,fd:1 -- output only, no console input.
#
# The usual con0=fd:0,fd:1 makes the run depend on what fd 0 happens to be. UML
# registers its console input fd with epoll, epoll_ctl rejects anything that is
# not pollable, and /dev/null is not pollable: run the identical boot with the
# harness's stdin attached and it reaches the marker in 84 lines, run it with
# stdin on /dev/null and it stops at 80 with every byte the guest wrote
# discarded. Nothing distinguishes that from a guest that never started.
#
# A gate is not interactive and has no use for console input, so the robust
# thing is not to ask for one. This also removes a whole class of "works when I
# run it by hand, fails from the script" from the phone harness.
CMDLINE="mem=$MEM panic=-1 con=null con0=null,fd:1 init=$INIT"
[ -n "$INITRD" ] && CMDLINE="$CMDLINE initrd=$INITRD"
[ -n "$UBD0" ]   && CMDLINE="$CMDLINE ubd0=$UBD0 root=/dev/ubda rw"
[ -n "$EXTRA_ARGS" ] && CMDLINE="$CMDLINE $EXTRA_ARGS"
echo "$CMDLINE" > "$OUT/cmdline"

log "gate=$GATE timeout=${TIMEOUT}s marker='$MARKER'"
log "cmdline: $CMDLINE"

# Capture on the device and pull the file afterwards, rather than reading adb's
# stdout. Streaming it loses the tail: `adb shell ... | tail` over a TCP
# connection returned a boot log that stopped at console initialisation, while
# the same boot captured to a file on the device was 90 lines long and ended in
# the success marker. Every gate that "failed" on this phone before the harness
# did this was passing; the measurement was truncating, and a truncated log is
# indistinguishable from a kernel that stopped.
#
# It also removes adb's exit status from the verdict, which is worth nothing
# anyway -- `adb shell` reports 0 regardless of what the remote command did
# unless both ends negotiate shell protocol v2. The marker decides, as the goal
# requires; RC is recorded but never trusted.
#
# A fresh copy of the disk image per run, for the same reason harness/boot.sh
# stages one: a gate that is killed by its timeout while root is mounted rw
# leaves the image dirty for whatever runs next.
if [ -n "$UBD0" ] && [ -f "$ROOT/rootfs/$UBD0" ]; then
	# Test for the pristine copy by asking for output, not by exit status:
	# `adb shell` returns 0 whatever the remote command did unless both ends
	# speak shell protocol v2, so `adb shell "[ -f x ]" || push` never pushes.
	if [ "$(adb shell "ls $DEV/$UBD0.orig 2>/dev/null | wc -l" | tr -dc 0-9)" != 1 ]; then
		log "staging pristine $UBD0 on the device (once, ~1.5 min)"
		adb push "$ROOT/rootfs/$UBD0" "$DEV/$UBD0.orig" >/dev/null || exit 2
	fi
	adb shell "cd $DEV && cp $UBD0.orig $UBD0 && sync" >/dev/null 2>&1
fi

# `| cat >` rather than `>`: UML registers its console fds with epoll, and
# epoll_ctl on a regular file fails with EPERM -- "epollctl add err fd 1,
# Operation not permitted" -- after which the console stops and the boot dies
# with no further output. A pipe is pollable, so the kernel keeps its console
# and cat does the writing to disk.
# No command grouping and no exit-code capture around the pipeline. Wrapping it
# in { ...; echo RC=$?; } changes the group's stdin, and UML reads its console
# from fd 0 -- the guest then boots but every byte its init writes is lost,
# which reads exactly like a guest that never ran. The goal says the verdict is
# a marker string and never an exit code, so there is nothing here worth
# distorting the pipeline to collect.
adb shell "cd $DEV && HOME=$DEV TMPDIR=/tmp timeout $TIMEOUT ./linux-um $CMDLINE 2>&1 | cat > boot.out" \
	>/dev/null 2>&1
adb pull "$DEV/boot.out" "$OUT/boot.log" >/dev/null 2>&1
RC=marker-only

tr -d '\r' < "$OUT/boot.log" > "$OUT/boot.txt" && mv "$OUT/boot.txt" "$OUT/boot.log"

grep -aE '^(BUG:|WARNING:|Oops|INFO: rcu|INFO: task .* blocked|Unable to handle kernel|kernel BUG at)' \
	"$OUT/boot.log" 2>/dev/null | sort | uniq -c | sort -rn > "$OUT/kernel-bugs.txt"
KBUGS=$(awk '{n += $1} END {print n+0}' "$OUT/kernel-bugs.txt")

if grep -qF "$MARKER" "$OUT/boot.log" 2>/dev/null; then
	[ "$KBUGS" -gt 0 ] && VERDICT=BUG || VERDICT=PASS
elif grep -qa "UMARM_TIMEOUT" "$OUT/boot.log"; then
	VERDICT=HANG
else
	VERDICT=FAIL
fi

{
	echo "gate=$GATE"
	echo "stamp=$STAMP"
	echo "verdict=$VERDICT"
	echo "rc=$RC"
	echo "marker=$MARKER"
	echo "cmdline=$CMDLINE"
	echo "kernel_bugs=$KBUGS"
	echo "bin_md5=$(md5sum "$BIN" | cut -d' ' -f1)"
	echo "host=android"
	sed 's/^/device: /' "$OUT/device.txt"
} > "$OUT/result.txt"

ln -sfn "$OUT" "$ROOT/artifacts/$GATE-latest"
log "verdict=$VERDICT rc=$RC kernel_bugs=$KBUGS -> $OUT"
[ "$VERDICT" = PASS ]
