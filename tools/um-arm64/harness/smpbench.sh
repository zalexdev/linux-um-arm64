#!/bin/bash
# SPDX-License-Identifier: GPL-2.0
#
# smpbench.sh -- what does giving the guest more CPUs actually buy?
#
# Run on a Raspberry Pi 5, not a phone, and each reason changes the answer
# rather than the convenience:
#
#   * The Pi's host pages are 16K, the same as the guest's, so every per-page
#     figure is one unit. On a 4K phone a 16K guest page hides a factor of four
#     inside the number, which is exactly the error that made the fault path
#     look far worse than it was.
#   * Docker runs natively on the Pi, so "docker build inside UML" has a
#     same-hardware reference. On a phone there is nothing to compare against.
#   * It is quiet: no app, no zygote, no handset thermal policy.
#
# The conditions are ONE kernel binary under different ncpus= values. NR_CPUS is
# only a compile-time ceiling; the count actually brought up is the ncpus= boot
# argument, clamped to it. So nothing is rebuilt between conditions and a
# difference cannot be a different compiler, config or binary.
#
# Two numbers are collected, and the gap between them is the point:
#
#   compile   a parallel build inside an already-built container. This is the
#             case SMP is supposed to help; if it does not scale here it will
#             not scale anywhere.
#   build     a full `docker build` of the same work. Deliberately not the same
#             number: a build also writes layers, and guest block I/O goes
#             through ubd, whose completions arrive as SIGIO. SIGIO is
#             process-directed, so the host delivers it to the thread group
#             leader -- CPU0 -- and extra CPUs cannot parallelise it. The gap
#             between compile and build is that effect, measured.
#
# Usage: HOST=magicjr KERNEL='~/umarm/linux-smp' ROUNDS=3 harness/smpbench.sh
set -uo pipefail

HOST=${HOST:-magicjr}
REMOTE=${REMOTE:-'$HOME/umarm'}
KERNEL=${KERNEL:-"$REMOTE/linux-smp"}
IMAGE=${IMAGE:-"$REMOTE/debian-docker.ext4"}
MEM=${MEM:-4096M}
ROUNDS=${ROUNDS:-3}
CPUSETS=${CPUSETS:-"1 2 4"}
UNITS=${UNITS:-24}        # translation units to compile; parallelism ceiling
OUT=${OUT:-/root/mlu-arm64/artifacts/smpbench-$(date -u +%Y%m%dT%H%M%SZ)}

mkdir -p "$OUT"
log() { printf '[smpbench] %s\n' "$*"; }
rsh() { timeout "${SSH_TIMEOUT:-1800}" ssh -o ConnectTimeout=10 "$HOST" "$@"; }

# ---- the guest-side script --------------------------------------------------
#
# Every wait here is bounded and prints the log it was waiting on when it gives
# up: a run that hangs must fail with the reason attached, not with silence.
# dockerd is killed at the end because init only powers off once the command
# returns, and a surviving background daemon holds the guest open forever.
cat > "$OUT/work.sh" <<WORKEOF
#!/bin/bash
export PATH=/usr/local/sbin:/usr/local/bin:/usr/sbin:/usr/bin:/sbin:/bin
NCPU=\$(nproc)
echo "SMPBENCH_NCPU=\$NCPU"

mount -t cgroup2 none /sys/fs/cgroup 2>/dev/null
mount -t tmpfs tmpfs /run 2>/dev/null
dockerd >/var/log/dockerd.log 2>&1 &
n=0
while [ \$n -lt 60 ]; do
	docker version >/dev/null 2>&1 && break
	n=\$((n+1)); sleep 1
done
if [ \$n -ge 60 ]; then
	echo "SMPBENCH_FAIL dockerd did not start"
	tail -20 /var/log/dockerd.log
	pkill -9 dockerd; exit 1
fi
echo "SMPBENCH_DOCKERD_UP=\${n}s"

mkdir -p /tmp/w && cd /tmp/w
# Independent translation units: the only limit on parallelism is CPUs, and
# nothing is fetched -- the base image is already in the image store.
i=0
while [ \$i -lt $UNITS ]; do
	{
		echo '#include <math.h>'
		j=0
		while [ \$j -lt 40 ]; do
			printf 'double f%s_%s(double x){double s=0;' "\$i" "\$j"
			printf 'for(int k=0;k<64;k++)s+=sin(x*k)*cos(x/(k+1));'
			printf 'return s;}\\n'
			j=\$((j+1))
		done
	} > u\$i.c
	i=\$((i+1))
done
printf 'all: \$(patsubst %%.c,%%.o,\$(wildcard *.c))\n' > Makefile

cat > Dockerfile <<'DEOF'
FROM debian:bookworm-slim
WORKDIR /src
COPY . /src/
RUN make -j\$(nproc) 2>&1 | tail -1
DEOF

# compile: parallel work only, inside a container that already exists.
# Nanoseconds and bash integer arithmetic: the image has no bc, and the first
# run of this discovered that by printing an empty timing for every condition.
t0=\$(date +%s%N)
docker run --rm -v /tmp/w:/src -w /src debian:bookworm-slim \\
	sh -c 'make -j\$(nproc) >/dev/null 2>&1' >/dev/null 2>&1
t1=\$(date +%s%N)
echo "SMPBENCH_COMPILE_MS=\$(( (t1 - t0) / 1000000 ))"
rm -f /tmp/w/*.o

# build: the same work plus layer writes, which is what was asked about.
t0=\$(date +%s%N)
docker build -q -t smpbench:t . >/dev/null 2>&1
t1=\$(date +%s%N)
echo "SMPBENCH_BUILD_MS=\$(( (t1 - t0) / 1000000 ))"
docker rmi -f smpbench:t >/dev/null 2>&1

# Where the interrupts landed. If they are all on CPU0 the I/O half of the
# build cannot have scaled, whatever the wall clock says.
echo "SMPBENCH_IRQ_BEGIN"
cat /proc/interrupts 2>/dev/null | head -12
echo "SMPBENCH_IRQ_END"

pkill -9 dockerd 2>/dev/null; pkill -9 containerd 2>/dev/null
echo "SMPBENCH_DONE"
WORKEOF

# ---- provenance -------------------------------------------------------------
{
	echo "### run $(date -u +%Y-%m-%dT%H:%M:%SZ)"
	echo "### git HEAD: $(git -C /root/mlu-arm64/linux rev-parse --short HEAD 2>/dev/null)"
	echo "### rounds: $ROUNDS  cpusets: $CPUSETS  units: $UNITS  mem: $MEM"
	echo
	rsh 'uname -a; echo "pagesize: $(getconf PAGESIZE)"; echo "host cpus: $(nproc)"; \
	     lscpu | grep -E "Model name"; \
	     echo "governor: $(cat /sys/devices/system/cpu/cpu0/cpufreq/scaling_governor)"; \
	     vcgencmd measure_temp 2>/dev/null; docker --version'
	echo "### kernel"
	rsh "md5sum $KERNEL; file -b $KERNEL | cut -c1-90"
} > "$OUT/manifest.txt" 2>&1

rsh "mkdir -p $REMOTE/share" >/dev/null 2>&1
dest=$(rsh "echo $REMOTE")/share/cmd
scp -q "$OUT/work.sh" "$HOST:$dest" ||
	{ echo "staging failed" >&2; exit 2; }
log "staged; manifest in $OUT/manifest.txt"

# ---- run --------------------------------------------------------------------
for r in $(seq 1 "$ROUNDS"); do
	for n in $CPUSETS; do
		log "round $r/$ROUNDS  ncpus=$n"
		rsh "cd $REMOTE && HOME=\$PWD timeout 1500 $KERNEL \
			mem=$MEM ubd0=$IMAGE root=/dev/ubda rw init=/umarm-init \
			umarm.share=$REMOTE/share ncpus=$n seccomp=auto \
			panic=-1 con=null con0=null,fd:1 2>&1 | cat" \
			> "$OUT/ncpus$n.$r" 2>&1
	done
done

# ---- report -----------------------------------------------------------------
{
	echo "=== medians over $ROUNDS rounds (seconds) ==="
	printf '%-8s %10s %10s %8s\n' ncpus compile_ms build_ms guest_nproc
	for n in $CPUSETS; do
		for m in COMPILE_MS BUILD_MS; do
			vals=$(grep -ah "SMPBENCH_$m=" "$OUT"/ncpus$n.* 2>/dev/null | cut -d= -f2)
			med=$(printf '%s\n' $vals | sort -n |
				awk '{a[NR]=$1}
				     END{if(NR)print (NR%2)?a[(NR+1)/2]:(a[NR/2]+a[NR/2+1])/2}')
			eval "med_$m=\$med"
		done
		np=$(grep -ah "SMPBENCH_NCPU=" "$OUT"/ncpus$n.1 2>/dev/null | cut -d= -f2 | head -1)
		printf '%-8s %10s %10s %8s\n' "$n" \
			"${med_COMPILE_MS:-?}" "${med_BUILD_MS:-?}" "${np:-?}"
	done
	echo
	echo "=== where interrupts landed (ncpus=4, round 1) ==="
	sed -n '/SMPBENCH_IRQ_BEGIN/,/SMPBENCH_IRQ_END/p' \
		"$OUT"/ncpus4.1 2>/dev/null
} | tee "$OUT/report.txt"

echo
echo "artifacts: $OUT"
