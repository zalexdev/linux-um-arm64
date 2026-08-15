#!/bin/bash
# Build a Debian bookworm arm64 rootfs image for gates 6-9.
#
# Two-stage debootstrap with qemu-user-static: qemu-user is unfit for *running*
# the UML port (see doc/00-environment-and-deviations.md) but is entirely
# appropriate for unpacking .debs, which is all the second stage does. Building
# this inside the TCG VM instead would work but takes an order of magnitude
# longer, and the result is byte-identical.
set -euo pipefail
ROOT=${ROOT:-/root/mlu-arm64}
IMG=${IMG:-$ROOT/rootfs/debian-bookworm-arm64.ext4}
SIZE_MB=${SIZE_MB:-6144}
DIR=${DIR:-$ROOT/rootfs/debian-bookworm}
MIRROR=${MIRROR:-http://deb.debian.org/debian}

rm -rf "$DIR"; mkdir -p "$DIR"
echo "[deb] stage 1"
debootstrap --arch=arm64 --foreign \
    --include=build-essential,gcc,make,libc6-dev,file,less,procps,psmisc,ca-certificates \
    bookworm "$DIR" "$MIRROR"

cp /usr/bin/qemu-aarch64-static "$DIR/usr/bin/"
echo "[deb] stage 2"
chroot "$DIR" /debootstrap/debootstrap --second-stage

echo "[deb] configure"
echo "umarm-guest" > "$DIR/etc/hostname"
printf '127.0.0.1\tlocalhost umarm-guest\n' > "$DIR/etc/hosts"
printf '/dev/ubda / ext4 defaults 0 1\n' > "$DIR/etc/fstab"
# Root with no password: this is a disposable test guest reached only through
# the UML console, never over a network.
chroot "$DIR" /bin/sh -c 'passwd -d root' >/dev/null 2>&1 || \
    sed -i 's|^root:[^:]*:|root::|' "$DIR/etc/shadow"
# A serial-style console on UML's tty0
printf 'console::respawn:/sbin/getty -L tty0 38400 linux\n' >> "$DIR/etc/inittab" 2>/dev/null || true

rm -f "$DIR/usr/bin/qemu-aarch64-static"

echo "[deb] image"
rm -f "$IMG"
dd if=/dev/zero of="$IMG" bs=1M count="$SIZE_MB" status=none
mkfs.ext4 -q -F -L umarm-debian "$IMG"
mkdir -p /mnt/debimg
mount -o loop "$IMG" /mnt/debimg
cp -a "$DIR"/. /mnt/debimg/
mkdir -p /mnt/debimg/{dev,proc,sys,host}
mknod /mnt/debimg/dev/console c 5 1 2>/dev/null || true
mknod /mnt/debimg/dev/null c 1 3 2>/dev/null || true
mknod /mnt/debimg/dev/tty c 5 0 2>/dev/null || true
sync; umount /mnt/debimg
ls -la "$IMG"
echo "[deb] done"
