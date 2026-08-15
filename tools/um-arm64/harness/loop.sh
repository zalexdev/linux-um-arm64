#!/bin/bash
# Run the gate matrix repeatedly and tally the results.
#
# The point is flakiness, not coverage. A port that passes each gate once has
# not been shown to work; it has been shown not to fail on the first try. An
# intermittent fault found later, during gate 7, costs far more than finding it
# here, because by then it is indistinguishable from a bug in whatever was
# written most recently -- and the natural response is to revert correct code.
#
# Deliberately serial: the run domain is one TCG VM, and running gates
# concurrently would create timeouts that look exactly like the flakes being
# hunted.
set -uo pipefail
ROOT=${ROOT:-/root/mlu-arm64}
HERE="$ROOT/harness"
N=${N:-100}
ONLY=${ONLY:-}
OUT="$ROOT/artifacts/loop-$(date -u +%Y%m%dT%H%M%SZ)"
mkdir -p "$OUT"
TSV="$OUT/results.tsv"
printf 'iter\tgate\tverdict\trc\tseconds\tartifact\n' > "$TSV"

# name | marker | init | ubd0 | initrd | extra | preload | timeout | every
#
# "every" samples the expensive gates. g4 spawns 2000 processes and fp runs
# ~8500 verify iterations; running both on every pass would put a 100-iteration
# loop past a day, which means it would not actually be run. The cheap gates --
# which is where boot and teardown flakes live -- run every time.
MATRIX=(
"g2mini|UMARM_GUEST_OK|/init||rootfs/initramfs-mini.cpio.gz|||180|1"
"g2alpine|UMARM_BOOT_OK|/init||rootfs/initramfs-alpine.cpio.gz|||240|1"
"g3|UMARM_BOOT_OK|/gate3-init|rootfs/alpine.ext4||rw||300|1"
"g5|UMARM_BOOT_OK|/gate5-init|rootfs/alpine.ext4||rw||300|1"
"g3noaslr|UMARM_BOOT_OK|/gate3-init|rootfs/alpine.ext4||rw|harness/probe/noaslr_preload.so|300|1"
"g4|UMARM_BOOT_OK|/gate4-init|rootfs/alpine.ext4||rw||900|5"
"fp|FPTORTURE_OK|/fp-init|rootfs/alpine.ext4||rw||900|5"
"fpnoaslr|FPTORTURE_OK|/fp-init|rootfs/alpine.ext4||rw|harness/probe/noaslr_preload.so|900|10"
)

declare -A pass fail
for i in $(seq 1 "$N"); do
    for row in "${MATRIX[@]}"; do
        IFS='|' read -r name marker init ubd0 initrd extra preload tmo every <<< "$row"
        [ -n "$ONLY" ] && [ "$ONLY" != "$name" ] && continue
        [ $(( i % ${every:-1} )) -eq 0 ] || continue
        t0=$SECONDS
        GATE="loop-$name" TIMEOUT="$tmo" MARKER="$marker" INIT="$init" \
        UBD0="${ubd0:+$ROOT/$ubd0}" INITRD="${initrd:+$ROOT/$initrd}" \
        EXTRA_ARGS="$extra" PRELOAD="${preload:+$ROOT/$preload}" \
        "$HERE/boot.sh" >/dev/null 2>&1
        rcbs=$?
        dt=$((SECONDS - t0))
        art=$(readlink -f "$ROOT/artifacts/loop-$name-latest")
        verdict=$(sed -n 's/^verdict=//p' "$art/result.txt" 2>/dev/null)
        rc=$(sed -n 's/^rc=//p' "$art/result.txt" 2>/dev/null)
        printf '%d\t%s\t%s\t%s\t%d\t%s\n' "$i" "$name" "${verdict:-NONE}" "${rc:-?}" "$dt" "$art" >> "$TSV"
        if [ "$verdict" = PASS ]; then
            pass[$name]=$(( ${pass[$name]:-0} + 1 ))
        else
            fail[$name]=$(( ${fail[$name]:-0} + 1 ))
            # Keep a failing run's full artifacts; everything else is
            # overwritten by the next iteration of the same gate.
            cp -a "$art" "$OUT/FAIL-$name-iter$i" 2>/dev/null
        fi
    done
    { echo "iterations_completed=$i"
      for k in "${!pass[@]}"; do echo "pass.$k=${pass[$k]}"; done
      for k in "${!fail[@]}"; do echo "fail.$k=${fail[$k]}"; done
    } > "$OUT/summary.txt"
done
echo "loop done -> $OUT"
cat "$OUT/summary.txt"
