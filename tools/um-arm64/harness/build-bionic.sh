#!/bin/bash
# Build UML/arm64 against bionic, for running inside an Android app.
#
# harness/build-android.sh already produces a binary that runs on the phone: a
# statically linked glibc one. That is enough for `adb shell`, and it is not
# enough for an app. An app process is started by zygote with a seccomp filter
# already installed, and glibc's startup issues calls that filter kills --
# rseq(2) and set_robust_list(2) -- so a glibc binary dies before main() with no
# output at all. bionic is the libc that filter was written for.
#
# The kernel notices bionic by itself: arch/arm64/Makefile.um asks the compiler
# whether __ANDROID__ is defined and adjusts from there. So the only thing this
# script does differently from build-android.sh is aim the compiler at Android,
# which it does by shimming --target on after kbuild's own -- clang takes the
# last one. Overriding CC instead would turn off kbuild's compiler probing.
#
# Every path below is derived from where this script actually is, or taken from
# the environment. It used to hardcode one particular home directory, which
# told anyone else reading it very little:
#
#   TREE=${TREE:-/root/mlu-arm64/linux}
#
# Nothing here wants to be root and nothing wants a specific location. Clone
# anywhere, run it from there.
set -uo pipefail

HERE=$(cd "$(dirname "$0")" && pwd)
# The harness lives at <repo>/harness in this tree and at
# <linux>/tools/um-arm64/harness when imported into a kernel tree; the kernel
# source is the parent of whichever one this is.
if [ -f "$HERE/../linux/Makefile" ]; then
    TREE=${TREE:-$(cd "$HERE/.." && pwd)/linux}      # <repo>/harness
elif [ -f "$HERE/../../../Makefile" ]; then
    TREE=${TREE:-$(cd "$HERE/../../.." && pwd)}      # <linux>/tools/um-arm64/harness
else
    TREE=${TREE:-$(cd "$HERE/.." && pwd)}
fi
if [ ! -f "$TREE/Makefile" ]; then
    echo "build-bionic.sh: no kernel tree at $TREE (set TREE=)" >&2
    exit 2
fi

ART=${ART:-$TREE/../artifacts}
API=${API:-30}   # oldest bionic declaring all UML uses: statx(30), getrandom(28)
JOBS=${JOBS:-$(nproc)}
O=${O:-${TMPDIR:-/tmp}/umarm-build-bionic}

# Find the NDK rather than insisting on one path. $NDK and the two variables
# Google's own tooling sets are checked first, then the usual install
# locations, newest last so the newest wins.
if [ -z "${NDK:-}" ]; then
    for c in "${ANDROID_NDK_HOME:-}" "${ANDROID_NDK_ROOT:-}" \
             "$HOME"/Android/Sdk/ndk/* "$HOME"/Library/Android/sdk/ndk/* \
             /usr/lib/android-ndk /opt/android-ndk* \
             "${TMPDIR:-/tmp}"/ndk/android-ndk-*; do
        [ -n "$c" ] && [ -x "$c/toolchains/llvm/prebuilt/linux-x86_64/bin/clang" ] \
            && NDK=$c
    done
fi
TOOL=${NDK:-}/toolchains/llvm/prebuilt/linux-x86_64/bin
if [ ! -x "$TOOL/clang" ]; then
    echo "build-bionic.sh: no Android NDK found. Set NDK=/path/to/android-ndk-rXX" >&2
    echo "  looked at \$NDK, \$ANDROID_NDK_HOME, \$ANDROID_NDK_ROOT," >&2
    echo "  ~/Android/Sdk/ndk/*, /opt/android-ndk*, ${TMPDIR:-/tmp}/ndk/*" >&2
    exit 2
fi

export KBUILD_BUILD_TIMESTAMP=${KBUILD_BUILD_TIMESTAMP:-"Thu Jan  1 00:00:00 UTC 1970"}
export KBUILD_BUILD_USER=um
export KBUILD_BUILD_HOST=arm64

SHIM=${SHIM:-$O/.bionic-shim}
mkdir -p "$SHIM"
cat > "$SHIM/clang" <<EOF
#!/bin/sh
# Append the Android target so it beats kbuild's --target=aarch64-linux-gnu.
exec $TOOL/clang "\$@" --target=aarch64-linux-android$API
EOF
# The link steps drive the compiler as the linker driver, so they need the shim
# too; everything else (llvm-ar, llvm-nm, ld.lld) is target-neutral.
cp "$SHIM/clang" "$SHIM/clang++"
chmod +x "$SHIM/clang" "$SHIM/clang++"
for t in ld.lld llvm-ar llvm-nm llvm-objcopy llvm-objdump llvm-readelf llvm-strip; do
    ln -sf "$TOOL/$t" "$SHIM/$t"
done
export PATH="$SHIM:$TOOL:$PATH"

# fixdep and friends run on the build machine, so they must not be built with
# the shim -- name the real compiler for them explicitly.
HOST_ARGS=(HOSTCC=/usr/bin/clang HOSTCXX=/usr/bin/clang++)

STAMP=$(date -u +%Y%m%dT%H%M%SZ)
LOG=$ART/build-bionic-$STAMP.log
mkdir -p "$O" "$ART"

{
    echo "### bionic build $STAMP"
    echo "### ndk=$NDK api=$API"
    echo "### head=$(git -C "$TREE" rev-parse --short HEAD 2>/dev/null)"
} > "$LOG"

# Static: Android has no /lib/ld-linux-aarch64.so.1, and an app's
# nativeLibraryDir is the only place it may exec from anyway.
make -C "$TREE" O="$O" ARCH=um SUBARCH=arm64 LLVM=1 "${HOST_ARGS[@]}" -j"$JOBS" \
     defconfig 2>&1 | tee -a "$LOG" >/dev/null
# UML_NET_VECTOR stays ON. It was disabled here because it used to
# "select MAY_HAVE_RUNTIME_DEPS", which is incompatible with STATIC_LINK -- but
# that select is gone (see arch/um/drivers/Kconfig), and leaving the disable
# behind cost the app its only network device: umnet hands the kernel a
# "vec0:transport=fd,fd=N" argument that nothing then claims, so the guest boots
# with no interface at all while passt sits on the other end of the socketpair
# waiting for frames that never come.
"$TREE"/scripts/config --file "$O/.config" -e STATIC_LINK -e UML_NET_VECTOR

# Extra fragments, e.g. EXTRA_CONFIG="config/usb-wifi.config config/docker.config".
# Merged before olddefconfig so their dependencies get pulled in properly.
for frag in ${EXTRA_CONFIG:-}; do
    [ -f "$frag" ] || { echo "no such config fragment: $frag" >&2; exit 2; }
    "$TREE"/scripts/kconfig/merge_config.sh -m -O "$O" "$O/.config" "$frag" \
        >/dev/null || exit 2
done
make -C "$TREE" O="$O" ARCH=um SUBARCH=arm64 LLVM=1 "${HOST_ARGS[@]}" -j"$JOBS" \
     olddefconfig 2>&1 | tee -a "$LOG" >/dev/null
make -C "$TREE" O="$O" ARCH=um SUBARCH=arm64 LLVM=1 "${HOST_ARGS[@]}" -j"$JOBS" 2>&1 | tee -a "$LOG"
rc=${PIPESTATUS[0]}

if [ "$rc" = 0 ] && [ -f "$O/linux" ]; then
    cp -f "$O/linux" "$ART/linux-bionic"
    cp -f "$O/arch/um/kernel/skas/stub_exe" "$ART/stub_exe_bionic" 2>/dev/null
    echo "linux-bionic: $(file -b "$ART/linux-bionic" | cut -c1-90)"
    echo "md5=$(md5sum "$ART/linux-bionic" | cut -c1-8) stub=$(md5sum "$ART/stub_exe_bionic" 2>/dev/null | cut -c1-8)"
fi
echo "bionic build rc=$rc log=$LOG"
exit "$rc"
