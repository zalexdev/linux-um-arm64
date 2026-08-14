#!/bin/sh
# SPDX-License-Identifier: GPL-2.0
#
# Emit the offset of each exported vDSO symbol as a #define, so the UML kernel
# can point a returning signal handler at __kernel_rt_sigreturn without parsing
# the ELF image at runtime. Mirrors arch/arm64/kernel/vdso/gen_vdso_offsets.sh.

LC_ALL=C
sed -n -e 's/^\([0-9a-f]*\) . \(__kernel_[a-z0-9_]*\)$/\#define um_vdso_offset_\2\t0x\1/p'
