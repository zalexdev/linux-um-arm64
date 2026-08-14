/* SPDX-License-Identifier: GPL-2.0 */
/*
 * Linker script for the UML/arm64 vDSO.
 *
 * Deliberately minimal. The only thing UML's guest actually needs from a vDSO
 * is __kernel_rt_sigreturn (see arch/arm64/um/signal.c); it exports no time
 * functions, because a UML guest's clock_gettime is a syscall into the UML
 * kernel, which is a function call, not a trap -- a vDSO fast path would save
 * nothing and would have to be kept in sync with UML's timekeeping.
 */

#include <asm/page.h>

OUTPUT_FORMAT("elf64-littleaarch64", "elf64-bigaarch64", "elf64-littleaarch64")
OUTPUT_ARCH(aarch64)

SECTIONS
{
	/*
	 * The vDSO is mapped as a single page by arch_setup_additional_pages(),
	 * so everything must fit below PAGE_SIZE. PAGE_SIZE, not 4096: on a
	 * 16K-page host the guest's page size is 16K too.
	 */
	. = SIZEOF_HEADERS;

	.hash		: { *(.hash) }			:text
	.gnu.hash	: { *(.gnu.hash) }
	.dynsym		: { *(.dynsym) }
	.dynstr		: { *(.dynstr) }
	.gnu.version	: { *(.gnu.version) }
	.gnu.version_d	: { *(.gnu.version_d) }
	.gnu.version_r	: { *(.gnu.version_r) }

	.note		: { *(.note.*) }		:text	:note

	.dynamic	: { *(.dynamic) }		:text	:dynamic

	.eh_frame_hdr	: { *(.eh_frame_hdr) }		:text	:eh_frame_hdr
	.eh_frame	: { KEEP (*(.eh_frame)) }	:text

	.rodata		: { *(.rodata*) }		:text

	.text		: { *(.text*) }			:text	=0xd503201f

	/DISCARD/	: {
		*(.data .data.* .gnu.linkonce.d.* .sdata*)
		*(.bss .sbss .dynbss .dynsbss)
		*(.note.GNU-stack)
		*(.comment)
	}
}

PHDRS
{
	text		PT_LOAD		FLAGS(5) FILEHDR PHDRS;	/* PF_R|PF_X */
	dynamic		PT_DYNAMIC	FLAGS(4);		/* PF_R */
	note		PT_NOTE		FLAGS(4);		/* PF_R */
	eh_frame_hdr	PT_GNU_EH_FRAME;
}

VERSION
{
	LINUX_2.6.39 {
	global:
		__kernel_rt_sigreturn;
	local: *;
	};
}
