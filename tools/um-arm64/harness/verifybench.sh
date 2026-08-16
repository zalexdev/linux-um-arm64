#!/bin/bash
# A benchmark someone else can check.
#
# harness/phonebench.sh compares one kernel against native and proot. Comparing
# two *kernels* with it means two runs, and then the honest answer to "is this
# change faster" is "probably, if the machine did not move between them" -- which
# is exactly the question that already invalidated three tables on this project
# (a runaway process, a frequency ramp, a stale binary). Controls catch drift
# but cannot remove it.
#
# So: every kernel is a condition inside ONE run, interleaved round-robin with
# native and proot. A difference between two kernels here cannot be explained by
# thermal drift, governor state or background load, because they are separated
# by seconds and by nothing else. Cross-run comparison is not needed and is not
# offered.
#
# Everything a reader needs to disbelieve the result is recorded next to it:
#   manifest.txt  git commit, md5 and file(1) of every binary, device
#                 properties, kernel, page size, governor, battery, thermals
#   env-before/after  clock and temperature of every core around the run
#   <cond>.<round>   the raw transcript of every single measurement
#   results.tsv   one row per (condition, round, metric); the table is derived
#                 from this file and nothing else
#
# The rules from phonebench.sh carry over unchanged: pinned to cores 4-5 with a
# frequency keeper on core 6 (they share a cpufreq policy), interleaved rather
# than blocked, medians not means, and `compute` as a validator that voids the
# whole table if the conditions disagree.
set -uo pipefail

ROOT=${ROOT:-/root/mlu-arm64}
DEV=/data/local/tmp
MASK=${MASK:-30}
KEEPMASK=${KEEPMASK:-40}
ROUNDS=${ROUNDS:-7}
OUT=${OUT:-$ROOT/artifacts/verifybench-$(date -u +%Y%m%dT%H%M%SZ)}

# Which interception modes to measure: "sec ptr" (default), or one of them.
#
# This is a correctness knob, not a convenience one. Every condition doubles the
# wall time of a round, ptrace conditions are the slow ones (20us a syscall
# against 2), and the run has to finish before the phone's thermal state moves
# or the validator throws the whole table away -- which is exactly what happened
# to two runs of three kernels x two modes: median compute 0.90ns with a 1.14ns
# outlier, 27% jitter, INVALID. The same comparison with MODES=sec passes.
#
# So: name the modes the question actually needs. A prefault window sweep is a
# seccomp question, and measuring ptrace alongside it does not make it more
# rigorous -- it makes it unmeasurable.
MODES=${MODES:-"sec ptr"}
for m in $MODES; do
	case "$m" in
	sec|ptr) ;;
	*) echo "verifybench: unknown mode '$m' (want sec and/or ptr)" >&2
	   exit 2 ;;
	esac
done

# "label=path" per kernel, in the order they should appear, and optionally
# "label=path%extra boot args".
#
# The second form is what lets the *same binary* be a condition twice under
# different command lines -- prefault=0 against the default, say, or a
# seccomp_spin sweep. Those are the cheapest experiments available, because
# they need no rebuild, and without this the harness could only compare
# binaries: the command line was hardcoded in run_uml.
#
#   KERNELS="base=/path/linux pf0=/path/linux%prefault=0"
#
# % rather than : as the separator, because a path may contain a colon and a
# boot argument certainly does (ubd0=...).
KERNELS=${KERNELS:?set KERNELS="label=/path/to/linux[%extra args] ..."}

# kernel_path/kernel_args - split one KERNELS entry
kernel_path() { local v=${1#*=}; printf '%s' "${v%%\%*}"; }
kernel_args() { local v=${1#*=}; case "$v" in *%*) printf '%s' "${v#*%}" ;; *) : ;; esac; }

mkdir -p "$OUT"
log() { printf '[verify] %s\n' "$*"; }

# ---- provenance -------------------------------------------------------------
{
	echo "### run $(date -u +%Y-%m-%dT%H:%M:%SZ)"
	echo "### host tree: $ROOT/linux"
	echo "### git HEAD: $(git -C "$ROOT/linux" rev-parse HEAD 2>/dev/null)"
	echo "### git describe: $(git -C "$ROOT/linux" describe --always --dirty 2>/dev/null)"
	echo "### rounds: $ROUNDS  mask: $MASK  keeper: $KEEPMASK"
	echo
	echo "### kernels under test"
	for k in $KERNELS; do
		lbl=${k%%=*}; path=$(kernel_path "$k")
		echo "  $lbl"
		echo "    path : $path"
		echo "    args : $(kernel_args "$k")"
		echo "    md5  : $(md5sum "$path" | cut -d' ' -f1)"
		echo "    size : $(stat -c%s "$path")"
		echo "    file : $(file -b "$path" | cut -c1-100)"
	done
	echo
	echo "### device"
	adb shell 'getprop ro.product.model; getprop ro.build.version.release; uname -a; getconf PAGESIZE; id' 2>/dev/null | tr -d '\r' | sed 's/^/  /'
	echo "### cpufreq policies"
	adb shell 'for p in /sys/devices/system/cpu/cpufreq/policy*/; do echo "$(basename $p) cpus=$(cat $p/related_cpus) gov=$(cat $p/scaling_governor) max=$(cat $p/cpuinfo_max_freq)"; done' 2>/dev/null | tr -d '\r' | sed 's/^/  /'
	echo "### battery"
	adb shell 'dumpsys battery | grep -iE "level|status|AC powered"' 2>/dev/null | tr -d '\r' | sed 's/^/  /'
	echo
	echo "### guest payload"
	echo "  perfbench md5: $(adb shell "md5sum $DEV/perfbench" 2>/dev/null | tr -d '\r')"
	echo "  initramfs md5: $(adb shell "md5sum $DEV/initramfs-perf.cpio.gz" 2>/dev/null | tr -d '\r')"
} > "$OUT/manifest.txt"

env_snapshot() {
	adb shell 'for c in 0 4 5 6 7; do echo -n "cpu$c "; cat /sys/devices/system/cpu/cpu$c/cpufreq/scaling_cur_freq 2>/dev/null || echo -; done
	           for z in /sys/class/thermal/thermal_zone*/; do t=$(cat $z/temp 2>/dev/null); n=$(cat $z/type 2>/dev/null); case $n in *cpu*) echo "$n $t";; esac; done' 2>/dev/null | tr -d '\r'
}
env_snapshot > "$OUT/env-before.txt"

# ---- stage the kernels ------------------------------------------------------
for k in $KERNELS; do
	lbl=${k%%=*}; path=$(kernel_path "$k")
	adb push "$path" "$DEV/k-$lbl" >/dev/null 2>&1 || { echo "push failed: $path" >&2; exit 2; }
done

# ---- the run ----------------------------------------------------------------
adb shell "nohup taskset $KEEPMASK sh -c 'while :; do :; done' >/dev/null 2>&1 &" >/dev/null 2>&1
stop_keeper() { adb shell "pkill -9 -f 'while :; do :; done'" >/dev/null 2>&1; }
trap stop_keeper EXIT INT TERM
sleep 5

run_native() { adb shell "cd $DEV && taskset $MASK ./perfbench --quick 2>&1"; }
run_proot() {
	adb shell "cd $DEV && TMPDIR=$DEV PROOT_LOADER=$DEV/prt/libexec/loader \
		LD_LIBRARY_PATH=$DEV/prt/lib taskset $MASK ./prt/proot -r alp /perfbench --quick 2>&1"
}
# run_uml <label> <seccomp on|off> [extra boot args]
run_uml() {
	adb shell "cd $DEV && HOME=$DEV TMPDIR=/tmp taskset $MASK timeout 300 ./k-$1 \
		mem=512M panic=-1 con=null con0=null,fd:1 \
		initrd=initramfs-perf.cpio.gz init=/init seccomp=$2 $3 2>&1 | cat"
}

CONDS="native proot"
for k in $KERNELS; do
	lbl=${k%%=*}
	for m in $MODES; do
		CONDS="$CONDS ${lbl}-${m}"
	done
done
echo "$CONDS" > "$OUT/conditions.txt"

for r in $(seq 1 "$ROUNDS"); do
	log "round $r/$ROUNDS"
	run_native > "$OUT/native.$r" 2>&1
	run_proot  > "$OUT/proot.$r"  2>&1
	for k in $KERNELS; do
		lbl=${k%%=*}; xargs=$(kernel_args "$k")
		for m in $MODES; do
			[ "$m" = sec ] && sc=on || sc=off
			run_uml "$lbl" "$sc" "$xargs" > "$OUT/${lbl}-${m}.$r" 2>&1
		done
	done
	sleep 3
done

env_snapshot > "$OUT/env-after.txt"
stop_keeper

# ---- results ----------------------------------------------------------------
python3 "$ROOT/harness/verifyreport.py" "$OUT" "$ROUNDS" | tee "$OUT/report.txt"
echo
echo "artifacts: $OUT"
