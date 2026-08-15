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
set -uo pipefail

TREE=${TREE:-/root/mlu-arm64/linux}
ART=${ART:-/root/mlu-arm64/artifacts}
NDK=${NDK:-/tmp/ndk/android-ndk-r27c}
# 30 is the oldest level whose bionic declares everything UML uses directly:
# statx (30), getrandom (28), futimes (26). The phone is API 35.
API=${API:-30}
JOBS=${JOBS:-$(nproc)}
O=${O:-/tmp/umarm-build-bionic}

TOOL=$NDK/toolchains/llvm/prebuilt/linux-x86_64/bin
if [ ! -x "$TOOL/clang" ]; then
    echo "no NDK at $NDK (set NDK=)" >&2
    exit 2
fi

export KBUILD_BUILD_TIMESTAMP=${KBUILD_BUILD_TIMESTAMP:-"Thu Jan  1 00:00:00 UTC 1970"}
export KBUILD_BUILD_USER=um
export KBUILD_BUILD_HOST=arm64

SHIM=/root/mlu-arm64/.bionic-shim
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
