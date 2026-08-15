#!/bin/bash
# Build umusb against bionic, for the app.
#
# umusb is the USB/IP server that hands the phone's USB WiFi adapter to the
# guest: the app asks the user for permission, gets an open usbfs descriptor
# out of UsbDeviceConnection.getFileDescriptor(), and passes the number to
# this helper, which drives USBDEVFS_* ioctls on it and speaks USB/IP to
# vhci_hcd in the guest.
#
# Bionic, static, and 16K-aligned for the same three reasons the other helpers
# are (see build-net-bionic.sh): an app process is started by zygote with a
# seccomp filter that kills glibc's start-up before main(), an app has no
# dynamic loader it may use, and Android 15 ships 16K-page kernels whose ELF
# loader will not map a 4K-aligned segment.
#
# The build refuses to install a binary whose protocol self-test fails, and
# runs that self-test twice: once natively, and once as the actual aarch64
# binary under qemu-user, which is the only way to check the packed-struct
# layouts on the ABI the phone will use.
set -uo pipefail

SRC=${SRC:-/root/mlu-arm64/harness/umusb.c}
ART=${ART:-/root/mlu-arm64/artifacts}
NDK=${NDK:-/tmp/ndk/android-ndk-r27c}
# Same level as the kernel and the other helpers: the oldest bionic that
# declares everything used directly. The phone is API 35.
API=${API:-30}
O=${O:-/tmp/umusb-build-bionic-$$}
PAGE=${PAGE:-16384}
QEMU=${QEMU:-qemu-aarch64-static}

TOOL=$NDK/toolchains/llvm/prebuilt/linux-x86_64/bin
if [ ! -x "$TOOL/clang" ]; then
    echo "no NDK at $NDK (set NDK=)" >&2
    exit 2
fi
CC="$TOOL/clang --target=aarch64-linux-android$API"

STAMP=$(date -u +%Y%m%dT%H%M%SZ)
LOG=$ART/build-usb-bionic-$STAMP.log
mkdir -p "$O" "$ART"

{
    echo "### umusb bionic build $STAMP"
    echo "### ndk=$NDK api=$API page=$PAGE src=$SRC"
} > "$LOG"

# check_bin() - Fail unless $1 is a helper an Android app can actually exec
#
# All three of these fail the same silent way -- the app execs the helper and
# gets nothing back at all -- so they are checked here rather than on the
# phone: a PT_INTERP is an interpreter the app cannot load, the wrong libc
# dies in zygote's seccomp filter before main(), and LOAD segments aligned
# below the device's page size make the ELF unloadable outright.
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

### the binary that ships
$CC -O2 -Wall -Wextra -static -Wl,-z,max-page-size="$PAGE" \
    -o "$O/umusb" "$SRC" 2>&1 | tee -a "$LOG"
rc=${PIPESTATUS[0]}
[ "$rc" = 0 ] || { echo "umusb build failed rc=$rc log=$LOG"; exit "$rc"; }

check_bin "$O/umusb" || exit 1

### self-test, natively first: a build host failure is the easiest to read
if command -v cc >/dev/null 2>&1; then
    cc -O2 -Wall -Wextra -o "$O/umusb_host" "$SRC" 2>&1 | tee -a "$LOG"
    rc=${PIPESTATUS[0]}
    [ "$rc" = 0 ] || { echo "host build failed rc=$rc log=$LOG"; exit "$rc"; }
    "$O/umusb_host" --selftest 2>&1 | tee -a "$LOG"
    rc=${PIPESTATUS[0]}
    [ "$rc" = 0 ] || { echo "host selftest failed rc=$rc log=$LOG"; exit "$rc"; }
fi

### and then the real binary, on the real ABI
#
# Worth the extra step: everything on the wire is a packed struct read into
# with memcpy, and the one thing a build-host run cannot tell you is whether
# the aarch64 compiler laid those out the way the guest kernel does.
if command -v "$QEMU" >/dev/null 2>&1; then
    "$QEMU" "$O/umusb" --selftest 2>&1 | tee -a "$LOG"
    rc=${PIPESTATUS[0]}
    [ "$rc" = 0 ] || { echo "aarch64 selftest failed rc=$rc log=$LOG"; exit "$rc"; }
else
    echo "### no $QEMU, aarch64 selftest skipped" | tee -a "$LOG"
fi

cp -f "$O/umusb" "$ART/umusb_bionic"
{
    echo "umusb_bionic: $(file -b "$ART/umusb_bionic" | cut -c1-90)"
    echo "md5=$(md5sum "$ART/umusb_bionic" | cut -c1-8)"
} | tee -a "$LOG"

# The app may only exec from its nativeLibraryDir, so the APK carries this as
# libumusb.so next to libuml.so and libumnet.so -- packaging is build-apk.sh's
# job, this only leaves the artifact where it looks for it.
rm -rf "$O"
echo "umusb bionic build rc=0 log=$LOG"
exit 0
