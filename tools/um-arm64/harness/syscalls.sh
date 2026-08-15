#!/bin/bash
# Record every host syscall ./linux issues, and summarise it.
#
# The goal asks for this as a deliverable, and it is the number that decides
# whether the binary can run inside an Android app sandbox: what UML asks of its
# host is exactly the surface that has to be permitted there.
#
# strace cannot produce it. UML is itself a ptrace tracer -- the stub processes
# are its tracees -- and a second tracer on the same processes conflicts;
# doc/30-syscall-budget.md records what that looks like. ftrace has no such
# problem, because it is not a tracer in the ptrace sense: raw_syscalls:sys_enter
# is a tracepoint, it costs the traced process nothing but a counter, and it sees
# the stub children too.
#
# Two things this gets right that are easy to get wrong:
#
#   * the pid filter is armed by the very process that then execs ./linux, using
#     `sh -c` so that $$ really is that process. An earlier version used $$
#     inside a bash subshell, where $$ is the *parent's* pid, and captured
#     thirteen syscalls belonging to the shell instead of a whole boot.
#
#   * the trace is drained through trace_pipe into a file for the duration,
#     rather than read out of the ring buffer afterwards. A UML boot issues
#     millions of syscalls and the ring buffer would drop most of them, which
#     would not be visible in the output -- it would just quietly produce a
#     smaller, wrong answer.
set -uo pipefail

ROOT=${ROOT:-/root/mlu-arm64}
HERE=$(cd "$(dirname "$0")" && pwd)
VM="$HERE/vm.sh"
OUT=${OUT:-$ROOT/artifacts/syscalls-$(date -u +%Y%m%dT%H%M%SZ)}
TIMEOUT=${TIMEOUT:-600}
CMDLINE=${CMDLINE:-"initrd=/var/tmp/umboot/initrd mem=512M panic=-1 init=/init con=null con0=fd:0,fd:1"}

mkdir -p "$OUT"

# Same lock boot.sh takes: there is one run domain, and a gate running
# concurrently would both perturb the trace and be perturbed by it.
exec 9>"${TMPDIR:-/tmp}/umarm-rundomain.lock"
flock 9

cat > "$OUT/capture.sh" <<EOF
#!/bin/sh
# Runs inside the run domain. See harness/syscalls.sh for why it is shaped so.
set -u
T=/sys/kernel/tracing
cd /var/tmp/umboot
[ -x linux ] || { echo "no staged kernel"; exit 2; }

echo 0 > \$T/tracing_on
echo 0 > \$T/events/enable
echo nop > \$T/current_tracer
echo > \$T/trace
echo 1 > \$T/options/event-fork
echo 1 > \$T/buffer_size_kb 2>/dev/null || true

# Drain continuously; the ring buffer is far too small for a whole boot.
: > /var/tmp/umboot/trace.raw
cat \$T/trace_pipe > /var/tmp/umboot/trace.raw &
DRAIN=\$!

# $$ inside 'sh -c' is this very process, which then becomes ./linux, so the
# filter covers the kernel from its first instruction. event-fork extends it to
# the stub children as they are created.
sh -c '
	echo \$\$ > '\$T'/set_event_pid
	echo 1 > '\$T'/events/raw_syscalls/sys_enter/enable
	echo 1 > '\$T'/tracing_on
	exec timeout --signal=KILL --kill-after=10 $TIMEOUT \
		./linux $CMDLINE
' > /var/tmp/umboot/trace-boot.log 2>&1 </dev/null
rc=\$?

echo 0 > \$T/tracing_on
echo 0 > \$T/events/raw_syscalls/sys_enter/enable
echo > \$T/set_event_pid
sleep 1
kill \$DRAIN 2>/dev/null
wait \$DRAIN 2>/dev/null

echo "RC=\$rc"
echo "trace_lines=\$(wc -l < /var/tmp/umboot/trace.raw)"
# Events the kernel dropped anyway, so the report can say so instead of
# silently under-reporting.
echo "overrun=\$(cat \$T/per_cpu/cpu*/stats 2>/dev/null | awk '/^overrun/{s+=\$2} END{print s+0}')"
grep -o 'NR [0-9]*' /var/tmp/umboot/trace.raw | sort | uniq -c | sort -rn \
	> /var/tmp/umboot/syscalls.txt
echo "distinct=\$(wc -l < /var/tmp/umboot/syscalls.txt)"
EOF

$VM scp "$OUT/capture.sh" /var/tmp/umboot/capture.sh >/dev/null || exit 2
$VM ssh 'chmod +x /var/tmp/umboot/capture.sh && /var/tmp/umboot/capture.sh' \
	> "$OUT/capture.log" 2>&1

$VM ssh 'cat /var/tmp/umboot/syscalls.txt'   > "$OUT/counts.txt" 2>/dev/null
$VM ssh 'cat /var/tmp/umboot/trace-boot.log' > "$OUT/boot.log"  2>/dev/null

cat "$OUT/capture.log"

# Numbers to names, from the same tree the kernel was built from, so the mapping
# cannot drift from the ABI actually in use.
python3 - "$OUT" <<'PY'
import os, re, sys, pathlib

out = pathlib.Path(sys.argv[1])
# The table kbuild generated for THIS build, not whatever <asm/unistd.h>
# resolves to. Preprocessing the source header instead finds the build host's
# own /usr/include/asm/unistd_64.h -- an x86_64 table -- and produces a syscall
# list whose numbers are right and whose every name is wrong, which is worse
# than no list at all.
tbl = {}
gen = pathlib.Path(os.environ.get(
    "UNISTD_H", "/tmp/umarm-build/arch/arm64/include/generated/uapi/asm/unistd_64.h"))
if not gen.exists():
    sys.exit(f"missing generated syscall table {gen}; build the kernel first")
for line in gen.read_text().splitlines():
    m = re.match(r"#define __NR_(\w+)\s+(\d+)$", line)
    if m:
        tbl.setdefault(int(m.group(2)), m.group(1))
if tbl.get(117) != "ptrace":
    sys.exit(f"syscall table does not look like aarch64 (117 = {tbl.get(117)})")

rows = []
for line in (out / "counts.txt").read_text().splitlines():
    m = re.match(r"\s*(\d+) NR (\d+)$", line)
    if m:
        rows.append((int(m.group(1)), int(m.group(2))))

log = (out / "capture.log").read_text()
def field(k):
    m = re.search(rf"^{k}=(\S+)$", log, re.M)
    return m.group(1) if m else "?"

with (out / "syscalls.md").open("w") as f:
    f.write("# Host syscalls issued by ./linux\n\n")
    f.write("Captured with ftrace `raw_syscalls:sys_enter`, pid-filtered from the\n")
    f.write("process that execs the kernel and extended to its children with\n")
    f.write("`event-fork`, so the stub processes are included and nothing is missed\n")
    f.write("between exec and enable. Drained through `trace_pipe` for the duration\n")
    f.write("rather than read from the ring buffer afterwards.\n\n")
    f.write(f"- distinct syscalls: **{len(rows)}**\n")
    f.write(f"- events recorded: {field('trace_lines')}\n")
    f.write(f"- events dropped by the ring buffer: {field('overrun')}\n\n")
    f.write("| count | nr | name |\n| ---: | ---: | --- |\n")
    for count, nr in rows:
        f.write(f"| {count} | {nr} | `{tbl.get(nr, '?')}` |\n")
print(f"{len(rows)} distinct syscalls -> {out}/syscalls.md")
PY

echo "trace in $OUT"
