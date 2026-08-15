#!/bin/bash
# Compile the same sources with the same compiler, once under UML and once
# natively, and compare the object code byte for byte.
#
# This is the strongest correctness evidence the project can produce, and it is
# strong precisely because it is transitive: for the hashes to match, every
# floating-point and SIMD register has to survive every context switch and every
# signal, TLS has to be right for every thread gcc spawns, and every byte gcc
# reads and writes has to arrive intact -- continuously, across a whole build. No
# targeted test covers that volume. A single differing bit anywhere fails it.
#
# The control is exact rather than approximate. The native run chroots into the
# *same disk image* the guest booted, so it is the same gcc binary, the same
# libc, the same headers, the same source at the same paths. The only variable
# left is whether the kernel underneath is UML/arm64 or the host's own.
#
# Run harness/boot.sh with the bitbuild gate first; this then does the native
# half and diffs the two.
set -uo pipefail

ROOT=${ROOT:-/root/mlu-arm64}
HERE=$(cd "$(dirname "$0")" && pwd)
VM="$HERE/vm.sh"
GUEST_LOG=${GUEST_LOG:-$ROOT/artifacts/bitguest-latest/boot.log}
OUT=${OUT:-$ROOT/artifacts/bitcmp-$(date -u +%Y%m%dT%H%M%SZ)}

mkdir -p "$OUT"

# One run domain, and this mounts the image the gates use.
exec 9>"${TMPDIR:-/tmp}/umarm-rundomain.lock"
flock 9

# Strip carriage returns. The guest's output reaches the log through a
# serial-style console that appends CR, so an otherwise identical hash arrives
# 65 characters long and compares unequal to the native one -- which reports a
# difference that does not exist. Getting a false negative from a comparison
# whose whole purpose is to be trusted is worse than getting no comparison.
sed -n '/UMARM_BITBUILD_START/,$p' "$GUEST_LOG" | tr -d '\r' |
	grep -E '^[0-9a-f]{64}  |^AGGREGATE |^gcc: ' > "$OUT/guest.txt"

if ! grep -q AGGREGATE "$OUT/guest.txt"; then
	echo "no guest results in $GUEST_LOG" >&2
	exit 2
fi

$VM ssh '
set -u
# Nothing may be using the image.
pkill -9 -x linux 2>/dev/null
sleep 1
rm -rf /mnt/bit && mkdir -p /mnt/bit
mount -o loop /var/tmp/umboot/ubd0 /mnt/bit || { echo "MOUNT_FAIL"; exit 2; }
mkdir -p /mnt/bit/tmp/bitsrc
cp /var/tmp/umboot/hostshare/bitsrc/*.c /mnt/bit/tmp/bitsrc/
cp /var/tmp/umboot/hostshare/bitbuild.sh /mnt/bit/tmp/
mount -t proc none /mnt/bit/proc 2>/dev/null
mount -t sysfs none /mnt/bit/sys 2>/dev/null
mount --bind /dev /mnt/bit/dev 2>/dev/null
chroot /mnt/bit /bin/sh /tmp/bitbuild.sh
rc=$?
umount /mnt/bit/dev /mnt/bit/proc /mnt/bit/sys 2>/dev/null
umount /mnt/bit 2>/dev/null
exit $rc
' 2>&1 | tr -d '\r' > "$OUT/native.txt"

echo "=== gcc ==="
grep '^gcc: ' "$OUT/guest.txt"  | sed 's/^/  guest  /'
grep '^gcc: ' "$OUT/native.txt" | sed 's/^/  native /'

g=$(grep '^AGGREGATE ' "$OUT/guest.txt"  | awk '{print $2}')
n=$(grep '^AGGREGATE ' "$OUT/native.txt" | awk '{print $2}')

echo
echo "=== aggregate sha256 over every .o and the linked binary ==="
echo "  guest  $g"
echo "  native $n"
echo

if [ -z "$n" ]; then
	echo "NATIVE RUN FAILED"; tail -5 "$OUT/native.txt"; exit 2
fi

# Per-file diff, so a mismatch names the translation unit rather than just failing.
join -j 2 \
	<(grep -E '^[0-9a-f]{64}  ' "$OUT/guest.txt"  | awk '{print $1, $2}' | sort -k2) \
	<(grep -E '^[0-9a-f]{64}  ' "$OUT/native.txt" | awk '{print $1, $2}' | sort -k2) \
	2>/dev/null | awk '$2 != $3 {print "  DIFFERS: " $1; n++} END {print "  files differing: " n+0}'

if [ "$g" = "$n" ]; then
	echo "BITCMP_IDENTICAL"
else
	echo "BITCMP_DIFFERENT"
fi
echo "results in $OUT"
