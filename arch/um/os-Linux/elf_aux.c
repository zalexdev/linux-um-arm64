// SPDX-License-Identifier: GPL-2.0
/*
 *  arch/um/kernel/elf_aux.c
 *
 *  Scan the ELF auxiliary vector provided by the host to extract
 *  information about vsyscall-page, etc.
 *
 *  Copyright (C) 2004 Fujitsu Siemens Computers GmbH
 *  Author: Bodo Stroesser (bodo.stroesser@fujitsu-siemens.com)
 */
#include <elf.h>
#include <stddef.h>
#include <init.h>
#include <elf_user.h>
#include <mem_user.h>
#include "internal.h"
#include <linux/swab.h>

#ifndef AT_MINSIGSTKSZ
#define AT_MINSIGSTKSZ 51
#endif

#if __BITS_PER_LONG == 64
typedef Elf64_auxv_t elf_auxv_t;
#else
typedef Elf32_auxv_t elf_auxv_t;
#endif

/* These are initialized very early in boot and never changed */
char * elf_aux_platform;
long elf_aux_hwcap;
/*
 * AT_HWCAP2 matters on arm64, where the feature space outgrew a single word
 * long ago (AT_HWCAP2 carries SVE2, MTE, the crypto extensions and more). x86
 * never populates it, so it simply stays zero there.
 */
long elf_aux_hwcap2;

/*
 * AT_MINSIGSTKSZ: the smallest alternate signal stack this host will accept as
 * sufficient, published by the kernel because on some architectures it is not a
 * constant.
 *
 * It is not a constant on arm64. setup_sigframe_layout() sizes a signal frame
 * from what the CPU supports and from what the signalled task has executed --
 * an SVE record scales with the vector length, and SME adds za and zt records
 * on top. Two run domains differing only in -cpu report 4720 and 9984 here,
 * for the same architecture and the same kernel.
 *
 * UML cares because the stub takes its signals on an alternate stack that is a
 * fixed-size member of struct stub_data, and the frame written there is sized
 * by the host, not by UML. Anything that reasons about that buffer must reason
 * from this number rather than from a measurement taken on one machine; see
 * check_stub_sigstack().
 *
 * Zero if the host does not publish it, which is the pre-5.5 case and also
 * everything that is not arm64.
 */
long elf_aux_min_sigstack;

__init void scan_elf_aux( char **envp)
{
	elf_auxv_t * auxv;

	while ( *envp++ != NULL) ;

	for ( auxv = (elf_auxv_t *)envp; auxv->a_type != AT_NULL; auxv++) {
		switch ( auxv->a_type ) {
			case AT_HWCAP:
				elf_aux_hwcap = auxv->a_un.a_val;
				break;
			case AT_HWCAP2:
				elf_aux_hwcap2 = auxv->a_un.a_val;
				break;
			case AT_MINSIGSTKSZ:
				elf_aux_min_sigstack = auxv->a_un.a_val;
				break;
			case AT_PLATFORM:
                                /* elf.h removed the pointer elements from
                                 * a_un, so we have to use a_val, which is
                                 * all that's left.
                                 */
				elf_aux_platform =
					(char *) (long) auxv->a_un.a_val;
				break;
		}
	}
}
