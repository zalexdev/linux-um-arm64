#!/bin/bash
# Compare, on one phone, the cost of the same workload run four ways.
#
# The first version of this measurement was worthless and the reason is worth
# keeping: the `compute` row -- pure userspace arithmetic with no kernel entry --
# came out as 1.270 / 6.012 / 0.838 ns across the three conditions, a sevenfold
# spread, with the guest apparently *faster* than native. Identical instruction
# streams cannot differ, so the numbers were measuring the machine's state, not
# the software: this is a big.LITTLE part (4x Cortex-A55 at 1.80 GHz, 3x A77 at
# 2.42, 1x A77 at 3.19) with schedutil, and the runs had landed on different
# clusters at different clock speeds and temperatures.
#
# Hence the three rules here:
#
#   1. Everything is pinned to cores 4-6 -- the three A77s that share a maximum
#      frequency. Not core 7, which boosts higher, and not 0-3, which are a
#      different microarchitecture. Affinity is inherited, so pinning the UML
#      process pins its stub children too, which is also what puts the kernel
#      thread and the stub on one cluster.
#   2. The conditions are interleaved, not run in blocks, so thermal drift and
#      frequency ramping hit all of them equally instead of penalising whichever
#      ran last.
#   3. `compute` is the validator. If it does not agree across all four columns
#      to within a couple of percent, the run is thrown away and no other row is
#      reported -- because if the machine was not in the same state, nothing
#      else in the table means anything either.
#   4. A busy loop is pinned to core 6 for the whole run, and measurement moves
#      to cores 4-5.
#
#      The governor is schedutil and there is no root on this device, so the
#      clock cannot simply be pinned. perfbench measures `compute` first, which
#      means that on an idle phone it measures the frequency ramp rather than
#      the machine: an otherwise clean run gave 1.195-1.334 ns/iter with an
#      11.6% spread, against 0.892 with 0.2% on a phone that happened to be
#      busy. Same binaries, same rootfs -- the difference was entirely which
#      clock the cluster was sitting at.
#
#      Cores 4, 5 and 6 share one cpufreq policy (policy4), so loading core 6
#      holds all three at their maximum. Measuring on 4-5 only keeps the keeper
#      out of the way of what is being timed. Core 7 is excluded as before: it
#      is the same microarchitecture but boosts higher, and cores 0-3 are A55s.
#
# The fourth column is proot, because that is the real alternative: the choice a
# user makes is between this kernel and proot-based Debian, not between this
# kernel and bare metal. Native is there to show the physical floor.
set -uo pipefail

ROOT=${ROOT:-/root/mlu-arm64}
DEV=/data/local/tmp
MASK=${MASK:-30}		# cores 4,5 as a hex affinity mask (toybox taskset)
KEEPMASK=${KEEPMASK:-40}	# core 6: the frequency keeper, see below
ROUNDS=${ROUNDS:-7}
OUT=${OUT:-$ROOT/artifacts/phonebench-$(date -u +%Y%m%dT%H%M%SZ)}

mkdir -p "$OUT"

run_native() {
	adb shell "cd $DEV && taskset $MASK ./perfbench --quick 2>&1"
}

run_proot() {
	adb shell "cd $DEV && TMPDIR=$DEV PROOT_LOADER=$DEV/prt/libexec/loader \
		LD_LIBRARY_PATH=$DEV/prt/lib taskset $MASK \
		./prt/proot -r alp /perfbench --quick 2>&1"
}

run_uml() {
	local mode=$1
	adb shell "cd $DEV && HOME=$DEV TMPDIR=/tmp taskset $MASK \
		timeout 300 ./linux-sec mem=512M panic=-1 con=null con0=null,fd:1 \
		initrd=initramfs-perf.cpio.gz init=/init $mode 2>&1 | cat"
}

# Pull one metric out of a perfbench transcript.
metric() { grep -a "^$2" "$1" | head -1 | awk '{print $2}'; }

# Start the keeper and make sure it dies with us, however we exit.
adb shell "nohup taskset $KEEPMASK sh -c 'while :; do :; done' >/dev/null 2>&1 &" \
	>/dev/null 2>&1
stop_keeper() { adb shell "pkill -9 -f 'while :; do :; done'" >/dev/null 2>&1; }
trap stop_keeper EXIT INT TERM

# Let schedutil notice the load and ramp before the first round is timed.
sleep 5

for r in $(seq 1 "$ROUNDS"); do
	echo "round $r/$ROUNDS"
	run_native            > "$OUT/native.$r"      2>&1
	run_proot             > "$OUT/proot.$r"       2>&1
	run_uml seccomp=off   > "$OUT/umlptrace.$r"   2>&1
	run_uml seccomp=on    > "$OUT/umlseccomp.$r"  2>&1
	# A pause between rounds rather than between conditions: the point is to
	# let the part cool a little without giving any one condition an
	# advantage over the others within a round.
	sleep 3
done

python3 - "$OUT" "$ROUNDS" <<'PY'
import sys, re, statistics, glob, os

out, rounds = sys.argv[1], int(sys.argv[2])
conds = [("native", "native"), ("proot", "proot"),
         ("umlptrace", "UML ptrace"), ("umlseccomp", "UML seccomp")]
rows = ["compute", "syscall", "openat", "fault", "forkexec"]

def vals(cond, row):
    got = []
    for f in sorted(glob.glob(os.path.join(out, cond + ".*"))):
        for line in open(f, errors="replace"):
            m = re.match(r'^' + row + r'\s+([0-9.]+)', line.strip())
            if m:
                got.append(float(m.group(1)))
                break
    return got

data = {c: {r: vals(c, r) for r in rows} for c, _ in conds}

# Rule 3: compute is the validator.
comp = {c: statistics.median(data[c]["compute"]) for c, _ in conds if data[c]["compute"]}
print("\n=== validator: compute (ns/iter, median of %d) ===" % rounds)
for c, label in conds:
    v = data[c]["compute"]
    if v:
        print("  %-12s %7.3f   (n=%d, min %.3f max %.3f)" %
              (label, statistics.median(v), len(v), min(v), max(v)))
if comp:
    lo, hi = min(comp.values()), max(comp.values())
    spread = (hi - lo) / lo * 100
    print("  spread: %.1f%%  -> %s" %
          (spread, "VALID" if spread < 5 else "INVALID, do not read the rows below"))

print("\n=== medians ===")
hdr = "%-10s" % "" + "".join("%14s" % l for _, l in conds)
print(hdr)
for r in rows:
    if r == "compute":
        continue
    line = "%-10s" % r
    for c, _ in conds:
        v = data[c][r]
        line += "%14s" % ("%.3f" % statistics.median(v) if v else "-")
    print(line)
print("\nunits: us/op except compute (ns/iter). raw transcripts in", out)
PY
