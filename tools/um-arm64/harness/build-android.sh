#!/bin/bash
# Build UML/arm64 binaries that can run on an Android phone.
#
# Two things make an Android build different from every other build here, and
# both are hard requirements rather than tuning:
#
#   * STATIC_LINK. UML normally links against the host's glibc. Android has
#     bionic and no /lib/ld-linux-aarch64.so.1 at all, so a dynamically linked
#     binary cannot even be exec'd -- the loader named in PT_INTERP does not
#     exist. A statically linked glibc binary is self-contained and needs
#     nothing from the host but syscalls, which is exactly the budget the goal
#     specifies.
#
#   * Page size is the host's, not ours to choose freely. The rule is guest
#     PAGE_SIZE >= host PAGE_SIZE; this phone reports 4096, so both 4 KB and
#     16 KB guests are legal. Both are built: 4 KB is the configuration with the
#     most validation behind it, 16 KB is what arm64_defconfig now produces.
#
# Everything else is deliberately identical to harness/build.sh, so a difference
# on the phone cannot be blamed on a different compiler or flags.
set -uo pipefail

TREE=${TREE:-/root/mlu-arm64/linux}
ART=${ART:-/root/mlu-arm64/artifacts}
JOBS=${JOBS:-$(nproc)}

export CCACHE_DIR=${CCACHE_DIR:-/root/mlu-arm64/.ccache}
export CCACHE_MAXSIZE=${CCACHE_MAXSIZE:-20G}
export KBUILD_BUILD_TIMESTAMP=${KBUILD_BUILD_TIMESTAMP:-"Thu Jan  1 00:00:00 UTC 1970"}
export KBUILD_BUILD_USER=um
export KBUILD_BUILD_HOST=arm64
export PATH="/root/mlu-arm64/.ccache-shim:$PATH"

build_one() {
	local name=$1 pagesize=$2 O=/tmp/umarm-build-$1
	local args=(-C "$TREE" O="$O" ARCH=um SUBARCH=arm64 LLVM=1)

	echo "=== $name (${pagesize}) ==="
	mkdir -p "$O"
	# Always from a fresh defconfig. olddefconfig on an existing tree preserves
	# a symbol that is already =y and never re-evaluates a default downward,
	# which has produced a wrong counterfactual here before.
	make "${args[@]}" arm64_defconfig </dev/null >/dev/null 2>&1 || return 1

	"$TREE/scripts/config" --file "$O/.config" -e STATIC_LINK
	# UML_NET_VECTOR selects MAY_HAVE_RUNTIME_DEPS, which STATIC_LINK depends on
	# being unset -- it wants getaddrinfo, i.e. NSS, i.e. dlopen at run time.
	"$TREE/scripts/config" --file "$O/.config" -d UML_NET_VECTOR
	case "$pagesize" in
	4k)  "$TREE/scripts/config" --file "$O/.config" -e PAGE_SIZE_4KB  -d PAGE_SIZE_16KB ;;
	16k) "$TREE/scripts/config" --file "$O/.config" -e PAGE_SIZE_16KB -d PAGE_SIZE_4KB ;;
	esac

	# </dev/null on every config step: a new symbol makes syncconfig prompt, and
	# a prompt with no stdin blocks forever. One such hang cost 1h37m here.
	make "${args[@]}" olddefconfig </dev/null >/dev/null 2>&1 || return 1

	# Verify the two things that matter actually took, before spending a build.
	grep -q '^CONFIG_STATIC_LINK=y' "$O/.config" || { echo "  STATIC_LINK did not stick"; return 1; }
	grep -q "^CONFIG_PAGE_SIZE_${pagesize^^}B=y" "$O/.config" || { echo "  page size did not stick"; return 1; }

	make "${args[@]}" -j"$JOBS" </dev/null > "$ART/build-android-$name.log" 2>&1 || {
		echo "  BUILD FAILED, tail:"; tail -20 "$ART/build-android-$name.log"; return 1; }

	cp -f "$O/linux" "$ART/linux-android-$name"
	cp -f "$O/.config" "$ART/config-android-$name"
	echo "  $(file -b "$ART/linux-android-$name" | cut -c1-90)"
	echo "  md5=$(md5sum "$ART/linux-android-$name" | cut -c1-8) size=$(stat -c%s "$ART/linux-android-$name")"
}

rc=0
build_one 4k 4k   || rc=1
build_one 16k 16k || rc=1
exit $rc
