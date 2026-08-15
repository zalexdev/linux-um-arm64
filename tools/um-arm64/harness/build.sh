#!/bin/bash
# Build ARCH=um SUBARCH=arm64 with clang.
#
# This is the ONLY place that knows the build domain is x86_64. Upstream already
# maps SUBARCH=arm64 to --target=aarch64-linux-gnu for ARCH=um in
# scripts/Makefile.clang, so on a native arm64 host this script's command line is
# byte-identical minus nothing at all -- there are no host-specific flags here.
#
# Output tree lives in tmpfs; ccache is shared across worktrees.
set -uo pipefail

TREE=${TREE:-/root/mlu-arm64/linux}
O=${O:-/tmp/umarm-build}
ART=${ART:-/root/mlu-arm64/artifacts}
JOBS=${JOBS:-$(nproc)}
TARGET=${1:-linux}

export CCACHE_DIR=${CCACHE_DIR:-/root/mlu-arm64/.ccache}
export CCACHE_MAXSIZE=${CCACHE_MAXSIZE:-20G}
export KBUILD_BUILD_TIMESTAMP=${KBUILD_BUILD_TIMESTAMP:-"Thu Jan  1 00:00:00 UTC 1970"}
export KBUILD_BUILD_USER=um
export KBUILD_BUILD_HOST=arm64

mkdir -p "$O" "$ART" "$CCACHE_DIR"

# ccache shim: kbuild hardcodes CC=clang under LLVM=1, so intercept via PATH
# rather than by overriding CC (which would also disable kbuild's compiler
# capability probing).
SHIM=/root/mlu-arm64/.ccache-shim
if [ ! -x "$SHIM/clang" ]; then
    mkdir -p "$SHIM"
    for t in clang clang++; do
        printf '#!/bin/sh\nexec ccache /usr/bin/%s "$@"\n' "$t" > "$SHIM/$t"
        chmod +x "$SHIM/$t"
    done
fi
export PATH="$SHIM:$PATH"

MAKE_ARGS=(
    -C "$TREE"
    O="$O"
    ARCH=um
    SUBARCH=arm64
    LLVM=1
    -j"$JOBS"
)

STAMP=$(date -u +%Y%m%dT%H%M%SZ)
LOG="$ART/build-$STAMP.log"

{
    echo "### build $STAMP"
    echo "### tree=$TREE head=$(git -C "$TREE" rev-parse --short HEAD 2>/dev/null) dirty=$(test -n "$(git -C "$TREE" status --porcelain 2>/dev/null)" && echo yes || echo no)"
    echo "### make ${MAKE_ARGS[*]} $TARGET"
    echo "### clang=$(clang --version | head -1)"
} > "$LOG"

make "${MAKE_ARGS[@]}" "$TARGET" 2>&1 | tee -a "$LOG"
rc=${PIPESTATUS[0]}

echo "### exit=$rc" >> "$LOG"
ln -sf "$LOG" "$ART/build-latest.log"

if [ -f "$O/linux" ]; then
    cp -f "$O/.config" "$ART/config-$STAMP" 2>/dev/null
    # Publish the binary the gates actually run. Without this the copy in
    # artifacts/ is whatever was last put there by hand, and every gate
    # silently reports on a kernel that is not the one just built -- a
    # failure mode with no symptom, since a stale binary boots perfectly.
    if [ "$rc" = 0 ] && [ "$TARGET" = linux ]; then
        cp -f "$O/linux" "$ART/linux-$STAMP" &&
            ln -sfn "$ART/linux-$STAMP" "$ART/linux-latest"
    fi
    echo "linux: $(file -b "$O/linux" | cut -c1-100)"
    echo "published: $ART/linux-latest -> $(readlink "$ART/linux-latest" 2>/dev/null)"
fi
echo "build rc=$rc log=$LOG"
exit "$rc"
