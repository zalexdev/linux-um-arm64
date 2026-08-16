#!/bin/bash
# SPDX-License-Identifier: GPL-2.0
#
# Wait for the SMP kernel to finish building, put it on the Pi, prove SMP
# actually comes up there, and only then spend time on the real measurement.
#
# The smoke test is not ceremony. ncpus= is a request that the kernel clamps --
# to NR_CPUS, and to 1 when the host can only do ptrace -- so a run can quietly
# produce four identical columns because every condition booted on one CPU. That
# failure looks exactly like "SMP does not help", which is the answer we are
# trying to measure, so it has to be excluded before the run rather than
# explained after it.
set -uo pipefail

HOST=${HOST:-magicjr}
TREE=${TREE:-/tmp/smp-tree}
O=${O:-/tmp/umarm-smp-glibc}
SRC=$O/linux
ROUNDS=${ROUNDS:-3}
JOBS=${JOBS:-$(nproc)}

# Build here rather than waiting for a build someone else started. Waiting on
# another process by name is how the first attempt at this deadlocked: the
# pattern "O=/tmp/umarm-smp" appears in the waiting shell's OWN command line, so
# pgrep matched the waiter and the loop could never end. Owning the build has no
# such failure mode, and it also means the kernel under test is the one this
# script configured.
#
# STATIC_LINK because the binary is built here and run on the Pi: a dynamic link
# would bind to this machine's glibc and depend on the Pi having the same one.
if [ ! -f "$SRC" ]; then
	echo "building $SRC"
	M=(-C "$TREE" O="$O" ARCH=um SUBARCH=arm64 LLVM=1 -j"$JOBS")
	make "${M[@]}" defconfig >/dev/null 2>&1 || { echo "FAIL: defconfig"; exit 1; }
	"$TREE"/scripts/config --file "$O/.config" -e STATIC_LINK -e UML_NET_VECTOR
	for f in /root/mlu-arm64/config/page16k.config \
		 /root/mlu-arm64/config/docker.config \
		 "$TREE"/smp16.config; do
		"$TREE"/scripts/kconfig/merge_config.sh -m -O "$O" "$O/.config" "$f" \
			>/dev/null 2>&1 || { echo "FAIL: merging $f"; exit 1; }
	done
	make "${M[@]}" olddefconfig >/dev/null 2>&1
	if ! make "${M[@]}" > /tmp/smpbuild.log 2>&1; then
		echo "FAIL: build; tail:"; tail -15 /tmp/smpbuild.log; exit 1
	fi
fi
[ -f "$SRC" ] || { echo "FAIL: no kernel at $SRC"; exit 1; }
grep -E "^CONFIG_(SMP|NR_CPUS|STATIC_LINK)=" "$O/.config"
echo "built: $(file -b "$SRC" | cut -c1-80)"

# Strip: the debug build is ~90MB and the link goes over a tunnel.
llvm-strip -s -o /tmp/linux-smp "$SRC" 2>/dev/null ||
	aarch64-linux-gnu-strip -s -o /tmp/linux-smp "$SRC" 2>/dev/null ||
	cp -f "$SRC" /tmp/linux-smp
scp -q -o ConnectTimeout=15 /tmp/linux-smp "$HOST:umarm/linux-smp" || {
	echo "FAIL: could not copy the kernel to $HOST"; exit 1; }
ssh "$HOST" 'chmod +x ~/umarm/linux-smp; ls -l ~/umarm/linux-smp'

# --- smoke test: does it boot, and does it really bring up 4 CPUs? ---
ssh "$HOST" 'mkdir -p ~/umarm/share; cat > ~/umarm/share/cmd' <<'EOF'
echo "SMOKE_NPROC=$(nproc)"
echo "SMOKE_CPUS=$(ls -d /sys/devices/system/cpu/cpu[0-9]* 2>/dev/null | wc -l)"
EOF

smoke=$(timeout 180 ssh "$HOST" 'cd ~/umarm && HOME=$PWD timeout 150 ./linux-smp \
	mem=2048M ubd0=$PWD/debian-docker.ext4 root=/dev/ubda rw init=/umarm-init \
	umarm.share=$PWD/share ncpus=4 seccomp=auto \
	panic=-1 con=null con0=null,fd:1 2>&1 | cat' 2>&1)

printf '%s\n' "$smoke" | grep -aE "Brought up|SMOKE_|Userspace mode|panic" | head -8

np=$(printf '%s\n' "$smoke" | sed -n 's/.*SMOKE_NPROC=\([0-9]*\).*/\1/p' | head -1)
if [ "${np:-0}" -lt 2 ]; then
	echo "FAIL: guest came up on ${np:-0} CPU with ncpus=4 - not measuring."
	printf '%s\n' "$smoke" | tail -25
	exit 1
fi
echo "smoke OK: guest reports $np CPUs"

# --- the real thing ---
ROUNDS=$ROUNDS HOST=$HOST KERNEL='$HOME/umarm/linux-smp' \
	IMAGE='$HOME/umarm/debian-docker.ext4' REMOTE='$HOME/umarm' \
	bash /root/mlu-arm64/harness/smpbench.sh
