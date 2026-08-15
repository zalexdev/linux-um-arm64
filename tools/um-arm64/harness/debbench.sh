#!/bin/bash
# Time a real Debian workload under the four host-side configurations, and
# refuse to report a number for any run that did not actually do the work.
#
# The last version of this measurement produced a 4x "speedup" that was entirely
# fake: a stale UML process still held the flock on the disk image, so the run
# aborted during root mount and returned in 0.83s instead of doing anything. A
# failed run is *faster* than a successful one, which makes silent failure the
# most dangerous thing a benchmark can do -- it does not add noise, it invents
# an improvement.
#
# So every run here is checked against the expected output before its time is
# counted, and any run that fails the check is reported rather than dropped.
set -uo pipefail

DEV=/data/local/tmp/umdebian
REPS=${REPS:-7}
CMD=${CMD:-"dpkg -l | wc -l"}
EXPECT=${EXPECT:-214}

adb shell "pkill -9 -f linux-um; pkill -9 -f linux-sec" >/dev/null 2>&1
sleep 2

adb shell "printf '%s\n' '$CMD' > $DEV/share/cmd"

run_once() {
	local sc=$1 pin=$2 t0 t1 out
	t0=$(date +%s.%N)
	out=$(adb shell "cd $DEV && HOME=\$PWD TMPDIR=/tmp $pin ./linux-um \
		mem=1024M ubd0=debian.ext4 root=/dev/ubda rw init=/umarm-init \
		umarm.share=\$PWD/share panic=-1 con=null con0=null,fd:1 $sc 2>&1 | cat" 2>&1)
	t1=$(date +%s.%N)
	# The guest prints the command's output between these markers.
	if printf '%s' "$out" | grep -q "UMARM_EXIT=0" &&
	   printf '%s' "$out" | tr -d '\r' | grep -q "^$EXPECT$"; then
		echo "$(echo "$t1 - $t0" | bc) ok"
	else
		echo "0 FAILED"
	fi
}

printf '%-34s %s\n' "configuration" "median of $REPS (successful runs only)"
for cfg in "seccomp=off:" "seccomp=off:taskset 80" "seccomp=on:" "seccomp=on:taskset 80"; do
	IFS=: read -r sc pin <<<"$cfg"
	times=(); fails=0
	for ((i = 0; i < REPS; i++)); do
		r=$(run_once "$sc" "$pin")
		if [[ $r == *FAILED* ]]; then fails=$((fails + 1)); else times+=("${r% ok}"); fi
	done
	label="$sc ${pin:-unpinned}"
	if [ ${#times[@]} -eq 0 ]; then
		printf '%-34s ALL %d RUNS FAILED\n' "$label" "$REPS"
	else
		med=$(printf '%s\n' "${times[@]}" | sort -n |
			awk '{a[NR]=$1} END{printf "%.2f", a[int((NR+1)/2)]}')
		printf '%-34s %ss   (%d ok, %d failed)\n' "$label" "$med" "${#times[@]}" "$fails"
	fi
done
