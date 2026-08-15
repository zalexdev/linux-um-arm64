#!/bin/bash
# Build the two userspace networking helpers against bionic, for the app.
#
# The guest's network is umnet + passt: umnet reframes between UML's vector
# `fd` transport and qemu's length-prefixed socket protocol, passt is the
# userspace TCP/IP stack that turns guest packets into ordinary outbound
# sockets. Neither needs any privilege, which is the whole point -- but both
# are separate processes, and until now both were glibc binaries.
#
# That is why networking works under `adb shell` and not in the app. An app
# process is started by zygote with a seccomp filter already installed, and
# glibc's startup issues rseq(2) and set_robust_list(2), which that filter
# kills: a glibc helper dies before main() with status 159 and no output at
# all. So the helpers have to be bionic binaries, exactly like the kernel that
# harness/build-bionic.sh produces.
#
# Unlike the kernel, neither helper has a kbuild to shim, so this script drives
# the NDK compiler directly. What it has to work around instead is passt's
# source: it was written for glibc and musl, and bionic differs from both in
# several headers. Those differences are fixed in the sources themselves and
# kept as dist/passt-bionic.patch, which explains each hunk; this script only
# makes sure the patch is applied to whatever tree it builds.
#
# The result is two static binaries. Static because Android has no dynamic
# loader available to an app, and an app may only exec from its
# nativeLibraryDir anyway -- same reasoning as CONFIG_STATIC_LINK for the
# kernel.
set -uo pipefail

SRC=${SRC:-/tmp/passt}			# passt checkout, patched or pristine
UMNET=${UMNET:-/root/mlu-arm64/harness/umnet.c}
PATCH=${PATCH:-/root/mlu-arm64/dist/passt-bionic.patch}
ART=${ART:-/root/mlu-arm64/artifacts}
NDK=${NDK:-/tmp/ndk/android-ndk-r27c}
# Same level the kernel is built at, for the same reason: 30 is the oldest
# bionic that declares everything used directly. The phone is API 35.
API=${API:-30}
O=${O:-/tmp/umnet-build-bionic}

# The largest page size these binaries have to run on. It is used twice.
#
# The link needs it: ld defaults to 4K segment alignment, and the kernel's ELF
# loader mmaps each LOAD segment at its own page granularity, so a binary whose
# (p_vaddr - p_offset) is only 4K-aligned cannot be loaded at all on a 16K-page
# kernel -- and Android 15 devices ship those. The kernel image dodges this by
# linking at 64K; these two get told explicitly.
#
# passt also bakes PAGE_SIZE in at compile time, from the *build* host's
# getconf, which cannot answer for the phone. It uses it only to align pkt_buf,
# to round the log rotation size and to round the MSS offered to the guest, so
# a value larger than the target's is harmless where a smaller one under-aligns.
PAGE=${PAGE:-16384}

# Widen passt's own seccomp allowlist if bionic turns out to need a syscall
# glibc did not -- the symptom would be passt dying on SIGSYS right after
# start-up, and the likely candidates are the ones Android's malloc uses:
# EXTRA_SYSCALLS="madvise mmap munmap mprotect". Empty by default, because
# every entry weakens the sandbox and no addition has been needed: the same
# sources built for x86_64-Android install the filter, survive it, answer ARP
# and complete a DHCP handshake on this build machine.
EXTRA_SYSCALLS=${EXTRA_SYSCALLS:-}

TOOL=$NDK/toolchains/llvm/prebuilt/linux-x86_64/bin
if [ ! -x "$TOOL/clang" ]; then
    echo "no NDK at $NDK (set NDK=)" >&2
    exit 2
fi
CC="$TOOL/clang --target=aarch64-linux-android$API"

STAMP=$(date -u +%Y%m%dT%H%M%SZ)
LOG=$ART/build-net-bionic-$STAMP.log
mkdir -p "$O" "$ART"

{
    echo "### net helpers bionic build $STAMP"
    echo "### ndk=$NDK api=$API page=$PAGE"
    echo "### passt=$SRC head=$(git -C "$SRC" rev-parse --short HEAD 2>/dev/null)"
} > "$LOG"

# check_bin() - Fail unless $1 is a helper an Android app can actually exec
#
# All three of these fail the same silent way -- the app execs the helper and
# gets nothing, no output, no clue -- so check them here rather than on the
# phone: a PT_INTERP is an interpreter the app cannot load, the wrong libc dies
# in zygote's seccomp filter before main(), and LOAD segments aligned below the
# device's page size make the ELF unloadable outright.
check_bin() {
    local f=$1 align

    if "$TOOL/llvm-readelf" -l "$f" | grep -q INTERP; then
	echo "$f: has PT_INTERP, not usable in an app" >&2
	return 1
    fi
    case "$(file -b "$f")" in
    *"ARM aarch64"*"statically linked"*"for Android"*)	;;
    *)	echo "$f: unexpected: $(file -b "$f")" >&2; return 1 ;;
    esac
    for align in $("$TOOL/llvm-readelf" -l "$f" | awk '$1 == "LOAD" { print $NF }'); do
	if [ "$((align))" -lt "$PAGE" ]; then
	    echo "$f: LOAD aligned $align < $PAGE, will not load on a $PAGE-page kernel" >&2
	    return 1
	fi
    done
    return 0
}

### umnet -- one translation unit, nothing to work around
$CC -O2 -Wall -Wextra -static -Wl,-z,max-page-size="$PAGE" \
    -o "$O/umnet" "$UMNET" 2>&1 | tee -a "$LOG"
rc=${PIPESTATUS[0]}
[ "$rc" = 0 ] || { echo "umnet build failed rc=$rc log=$LOG"; exit "$rc"; }

### passt
#
# Build from a copy, never in $SRC: concurrent builds for other targets share
# that checkout, and passt builds in-tree.
rm -rf "$O/passt-src"
mkdir -p "$O/passt-src"
tar -C "$SRC" --exclude=.git -cf - . | tar -C "$O/passt-src" -xf -
cd "$O/passt-src" || exit 1

# A checkout that has been built in still holds those outputs, and the copy
# keeps their mtimes -- so make would happily ship a stale glibc binary, or
# worse, reuse a seccomp.h generated for another architecture, whose AUDIT_ARCH
# check kills passt on its first syscall. Delete them and let make regenerate.
rm -f passt pasta passt-repair pesto passt.avx2 pasta.avx2 \
      seccomp.h seccomp_repair.h seccomp_pesto.h

# Accept either a pristine or an already-patched tree, so this works both from
# the working checkout and from a fresh clone at the patch's base commit.
if patch -p1 -R --dry-run --silent < "$PATCH" >/dev/null 2>&1; then
    echo "### sources already carry $PATCH" | tee -a "$LOG"
elif patch -p1 --silent < "$PATCH" >/dev/null 2>&1; then
    echo "### applied $PATCH" | tee -a "$LOG"
else
    echo "$PATCH applies neither forwards nor backwards to $SRC" >&2
    exit 2
fi

# TARGET/ARCH: the Makefile derives both from `$(CC) -dumpmachine`, which for
# the NDK driver reports the *host*. ARCH in particular feeds seccomp.sh, which
# turns it into the AUDIT_ARCH_* the generated filter checks against.
#
# CFLAGS/CPPFLAGS land after the Makefile's own, which is what makes -static
# and the PAGE_SIZE override stick; -U first because a second -D of a macro
# already given on the command line is a redefinition.
VERSION=$(git -C "$SRC" describe --tags --always HEAD 2>/dev/null || echo unknown)
make CC="$CC" ARCH=aarch64 TARGET=aarch64-linux-android \
     VERSION="$VERSION-bionic" EXTRA_SYSCALLS="$EXTRA_SYSCALLS" \
     CFLAGS="-static -Wl,-z,max-page-size=$PAGE" \
     CPPFLAGS="-UPAGE_SIZE -DPAGE_SIZE=$PAGE" \
     passt 2>&1 | tee -a "$LOG"
rc=${PIPESTATUS[0]}
[ "$rc" = 0 ] || { echo "passt build failed rc=$rc log=$LOG"; exit "$rc"; }

check_bin "$O/umnet" && check_bin "$O/passt-src/passt" || exit 1

cp -f "$O/umnet" "$ART/umnet_bionic"
cp -f "$O/passt-src/passt" "$ART/passt_bionic"
{
    echo "umnet_bionic: $(file -b "$ART/umnet_bionic" | cut -c1-90)"
    echo "passt_bionic: $(file -b "$ART/passt_bionic" | cut -c1-90)"
    echo "md5=$(md5sum "$ART/umnet_bionic" | cut -c1-8)/$(md5sum "$ART/passt_bionic" | cut -c1-8)"
} | tee -a "$LOG"
echo "net helpers bionic build rc=0 log=$LOG"
exit 0
