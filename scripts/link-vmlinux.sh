#!/bin/sh
# SPDX-License-Identifier: GPL-2.0
#
# link vmlinux
#
# vmlinux is linked from the objects in vmlinux.a and $(KBUILD_VMLINUX_LIBS).
# vmlinux.a contains objects that are linked unconditionally.
# $(KBUILD_VMLINUX_LIBS) are archives which are linked conditionally
# (not within --whole-archive), and do not require symbol indexes added.
#
# vmlinux
#   ^
#   |
#   +--< vmlinux.a
#   |
#   +--< $(KBUILD_VMLINUX_LIBS)
#   |    +--< lib/lib.a + more
#   |
#   +-< ${kallsymso} (see description in KALLSYMS section)
#
# vmlinux version (uname -v) cannot be updated during normal
# descending-into-subdirs phase since we do not yet know if we need to
# update vmlinux.
# Therefore this step is delayed until just before final link of vmlinux.
#
# System.map is generated to document addresses of all kernel symbols

# Error out on error
set -e

LD="$1"
KBUILD_LDFLAGS="$2"
LDFLAGS_vmlinux="$3"
VMLINUX="$4"

is_enabled() {
	grep -q "^$1=y" include/config/auto.conf
}

# Nice output in kbuild format
# Will be supressed by "make -s"
info()
{
	printf "  %-7s %s\n" "${1}" "${2}"
}

# Link of vmlinux
# ${1} - output file
vmlinux_link()
{
	local output=${1}
	local objs
	local libs
	local ld
	local ldflags
	local ldlibs

	info LD ${output}

	# skip output file argument
	shift

	if is_enabled CONFIG_LTO_CLANG || is_enabled CONFIG_X86_KERNEL_IBT ||
	   is_enabled CONFIG_KLP_BUILD; then
		# Use vmlinux.o instead of performing the slow LTO link again.
		objs=vmlinux.o
		libs=
	else
		objs=vmlinux.a
		libs="${KBUILD_VMLINUX_LIBS}"
	fi

	if is_enabled CONFIG_GENERIC_BUILTIN_DTB; then
		objs="${objs} .builtin-dtbs.o"
	fi

	objs="${objs} .vmlinux.export.o"
	objs="${objs} init/version-timestamp.o"

	if [ "${SRCARCH}" = "um" ]; then
		# Rename the kernel symbols that the target libc also uses,
		# before it gets a chance to bind to them. Doing it here rather
		# than with -D during compilation keeps the preprocessor out of
		# it: these names also occur as bare tokens that other headers
		# paste into identifiers, and rewriting those breaks builds in
		# places that have nothing to do with libc. See the list itself
		# for which names and why.
		#
		# The list is derived here rather than only read from the file,
		# because which names collide is a property of the libc being
		# linked against and not of this tree. NDK r27c's bionic has no
		# sched_setattr, newer ones do, so a build that worked shipped
		# a link failure to anyone on a different NDK:
		#
		#	ld.lld: error: duplicate symbol: sched_setattr
		#	ld.lld: error: duplicate symbol: sync_file_range
		#
		# Deriving it means a new libc adds a rename instead of a bug
		# report. The checked-in file stays as the explanation and as
		# the fallback when nm or libc.a cannot be found.
		#
		# Both halves matter and they fail differently. A name libc
		# DEFINES is a duplicate-symbol error, loud and immediate. A
		# name libc only REFERENCES silently binds to the kernel's
		# definition -- that was sched_getaffinity, where bionic's
		# startup called into the kernel's implementation and touched a
		# task_struct that does not exist yet, for a SIGSEGV before
		# main() with no output at all.
		if [ -n "${UM_REDEF_SYMS}" ]; then
			um_redef=.tmp_um_redef_syms
			: > "${um_redef}"
			if [ -f "${UM_REDEF_SYMS}" ]; then
				sed -e 's/#.*//' -e '/^[[:space:]]*$/d' \
					"${UM_REDEF_SYMS}" > "${um_redef}"
			fi

			um_libc=$(${CC} ${KBUILD_CFLAGS} -print-file-name=libc.a \
				  2>/dev/null)
			if [ -n "${NM}" ] && [ -f "${um_libc}" ]; then
				# Anything the kernel defines...
				${NM} --defined-only --extern-only \
					${objs} ${libs} 2>/dev/null |
					awk '$2 ~ /^[TWDBR]$/ { print $3 }' |
					sort -u > .tmp_um_kdef
				# ...that libc defines or references. Undefined
				# entries print as "U name", defined ones as
				# "addr T name"; archive member headers end in
				# ':' and match neither.
				${NM} "${um_libc}" 2>/dev/null |
					awk '$1 == "U" { print $2 }
					     $2 ~ /^[TWDBR]$/ { print $3 }' |
					sort -u > .tmp_um_libc
				comm -12 .tmp_um_kdef .tmp_um_libc |
				while read -r sym; do
					grep -q "^${sym}[[:space:]]" \
						"${um_redef}" && continue
					echo "${sym} kernel_${sym}"
				done >> "${um_redef}"
				rm -f .tmp_um_kdef .tmp_um_libc
			fi

			if [ -s "${um_redef}" ]; then
				for a in ${objs} ${libs}; do
					case "${a}" in
					*.a|*.o)
						${OBJCOPY} \
						  --redefine-syms="${um_redef}" \
						  "${a}" || exit 1
						;;
					esac
				done
			fi
		fi

		wl=-Wl,
		ld="${CC}"
		ldflags="${CFLAGS_vmlinux}"
		# glibc keeps these in separate archives; bionic folds all
		# three into libc, so -lutil, -lrt and -lpthread name libraries
		# that do not exist and the link fails outright. Ask the
		# compiler which of them can actually be linked rather than
		# assuming a libc -- UML links with $(CC), so this is the same
		# driver that will do the real link.
		ldlibs=""
		for lib in util rt pthread; do
			if echo 'int main(void){return 0;}' | \
			   ${CC} ${CFLAGS_vmlinux} -x c - -l${lib} \
				 -o /dev/null >/dev/null 2>&1; then
				ldlibs="${ldlibs} -l${lib}"
			fi
		done
	else
		wl=
		ld="${LD}"
		ldflags="${KBUILD_LDFLAGS} ${LDFLAGS_vmlinux}"
		ldlibs=
	fi

	ldflags="${ldflags} ${wl}--script=${objtree}/${KBUILD_LDS}"

	# The kallsyms linking does not need debug symbols included.
	if [ -n "${strip_debug}" ] ; then
		ldflags="${ldflags} ${wl}--strip-debug"
	fi

	if [ -n "${generate_map}" ];  then
		ldflags="${ldflags} ${wl}-Map=vmlinux.map"
	fi

	${ld} ${ldflags} -o ${output}					\
		${wl}--whole-archive ${objs} ${wl}--no-whole-archive	\
		${wl}--start-group ${libs} ${wl}--end-group		\
		${kallsymso} ${btf_vmlinux_bin_o} ${arch_vmlinux_o} ${ldlibs}
}

# Create ${2}.o file with all symbols from the ${1} object file
kallsyms()
{
	local kallsymopt;

	if is_enabled CONFIG_KALLSYMS_ALL; then
		kallsymopt="${kallsymopt} --all-symbols"
	fi

	if is_enabled CONFIG_64BIT || is_enabled CONFIG_RELOCATABLE; then
		kallsymopt="${kallsymopt} --pc-relative"
	fi

	info KSYMS "${2}.S"
	scripts/kallsyms ${kallsymopt} "${1}" > "${2}.S"

	info AS "${2}.o"
	${CC} ${NOSTDINC_FLAGS} ${LINUXINCLUDE} ${KBUILD_CPPFLAGS} \
	      ${KBUILD_AFLAGS} ${KBUILD_AFLAGS_KERNEL} -c -o "${2}.o" "${2}.S"

	kallsymso=${2}.o
}

# Perform kallsyms for the given temporary vmlinux.
sysmap_and_kallsyms()
{
	mksysmap "${1}" "${1}.syms"
	kallsyms "${1}.syms" "${1}.kallsyms"

	kallsyms_sysmap=${1}.syms
}

# Create map file with all symbols from ${1}
# See mksymap for additional details
mksysmap()
{
	info NM ${2}
	${NM} -n "${1}" | sed -f "${srctree}/scripts/mksysmap" > "${2}"
}

sorttable()
{
	${NM} -S ${1} > .tmp_vmlinux.nm-sort
	${objtree}/scripts/sorttable -s .tmp_vmlinux.nm-sort ${1}
}

cleanup()
{
	rm -f .btf.*
	rm -f .tmp_vmlinux.nm-sort
	rm -f System.map
	rm -f vmlinux
	rm -f vmlinux.map
}

# Use "make V=1" to debug this script
case "${KBUILD_VERBOSE}" in
*1*)
	set -x
	;;
esac

if [ "$1" = "clean" ]; then
	cleanup
	exit 0
fi

${MAKE} -f "${srctree}/scripts/Makefile.build" obj=init init/version-timestamp.o

arch_vmlinux_o=
if is_enabled CONFIG_ARCH_WANTS_PRE_LINK_VMLINUX; then
	arch_vmlinux_o=arch/${SRCARCH}/tools/vmlinux.arch.o
fi

btf_vmlinux_bin_o=
btfids_vmlinux=
kallsymso=
strip_debug=
generate_map=

# Use "make UT=1" to trigger warnings on unused tracepoints
case "${WARN_ON_UNUSED_TRACEPOINTS}" in
*1*)
	${objtree}/scripts/tracepoint-update vmlinux.o
	;;
esac

if is_enabled CONFIG_KALLSYMS; then
	true > .tmp_vmlinux0.syms
	kallsyms .tmp_vmlinux0.syms .tmp_vmlinux0.kallsyms
fi

if is_enabled CONFIG_KALLSYMS || is_enabled CONFIG_DEBUG_INFO_BTF; then

	# The kallsyms linking does not need debug symbols, but the BTF does.
	if ! is_enabled CONFIG_DEBUG_INFO_BTF; then
		strip_debug=1
	fi

	vmlinux_link .tmp_vmlinux1
fi

if is_enabled CONFIG_DEBUG_INFO_BTF; then
	info BTF .tmp_vmlinux1
	if ! ${CONFIG_SHELL} ${srctree}/scripts/gen-btf.sh .tmp_vmlinux1; then
		echo >&2 "Failed to generate BTF for vmlinux"
		echo >&2 "Try to disable CONFIG_DEBUG_INFO_BTF"
		exit 1
	fi
	btf_vmlinux_bin_o=.tmp_vmlinux1.btf.o
	btfids_vmlinux=.tmp_vmlinux1.BTF_ids
fi

if is_enabled CONFIG_KALLSYMS; then

	# kallsyms support
	# Generate section listing all symbols and add it into vmlinux
	# It's a four step process:
	# 0)  Generate a dummy __kallsyms with empty symbol list.
	# 1)  Link .tmp_vmlinux1.kallsyms so it has all symbols and sections,
	#     with a dummy __kallsyms.
	#     Running kallsyms on that gives us .tmp_vmlinux1.kallsyms.o with
	#     the right size
	# 2)  Link .tmp_vmlinux2.kallsyms so it now has a __kallsyms section of
	#     the right size, but due to the added section, some
	#     addresses have shifted.
	#     From here, we generate a correct .tmp_vmlinux2.kallsyms.o
	# 3)  That link may have expanded the kernel image enough that
	#     more linker branch stubs / trampolines had to be added, which
	#     introduces new names, which further expands kallsyms. Do another
	#     pass if that is the case. In theory it's possible this results
	#     in even more stubs, but unlikely.
	#     KALLSYMS_EXTRA_PASS=1 may also used to debug or work around
	#     other bugs.
	# 4)  The correct ${kallsymso} is linked into the final vmlinux.
	#
	# a)  Verify that the System.map from vmlinux matches the map from
	#     ${kallsymso}.

	# The kallsyms linking does not need debug symbols included.
	strip_debug=1

	sysmap_and_kallsyms .tmp_vmlinux1
	size1=$(${CONFIG_SHELL} "${srctree}/scripts/file-size.sh" ${kallsymso})

	vmlinux_link .tmp_vmlinux2
	sysmap_and_kallsyms .tmp_vmlinux2
	size2=$(${CONFIG_SHELL} "${srctree}/scripts/file-size.sh" ${kallsymso})

	if [ $size1 -ne $size2 ] || [ -n "${KALLSYMS_EXTRA_PASS}" ]; then
		vmlinux_link .tmp_vmlinux3
		sysmap_and_kallsyms .tmp_vmlinux3
	fi
fi

strip_debug=

if is_enabled CONFIG_VMLINUX_MAP; then
	generate_map=1
fi

vmlinux_link "${VMLINUX}"

if is_enabled CONFIG_DEBUG_INFO_BTF; then
	info BTFIDS ${VMLINUX}
	${RESOLVE_BTFIDS} --patch_btfids ${btfids_vmlinux} ${VMLINUX}
fi

mksysmap "${VMLINUX}" System.map

if is_enabled CONFIG_BUILDTIME_TABLE_SORT; then
	info SORTTAB "${VMLINUX}"
	if ! sorttable "${VMLINUX}"; then
		echo >&2 Failed to sort kernel tables
		exit 1
	fi
fi

# step a (see comment above)
if is_enabled CONFIG_KALLSYMS; then
	if ! cmp -s System.map "${kallsyms_sysmap}"; then
		echo >&2 Inconsistent kallsyms data
		echo >&2 'Try "make KALLSYMS_EXTRA_PASS=1" as a workaround'
		exit 1
	fi
fi

# For fixdep
echo "${VMLINUX}: $0" > ".${VMLINUX}.d"
