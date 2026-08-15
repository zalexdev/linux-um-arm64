/* SPDX-License-Identifier: GPL-2.0 */
#include <asm/vmlinux.lds.h>
#include <asm/page.h>

OUTPUT_FORMAT(ELF_FORMAT)
OUTPUT_ARCH(ELF_ARCH)
ENTRY(_start)
jiffies = jiffies_64;

VERSION {
  {
    local: *;
  };
}

/*
 * One loadable segment for the whole binary.
 *
 * Without this, lld gives every permission group its own PT_LOAD and takes the
 * addresses from this script, so any group that starts mid-page yields two
 * segments sharing a page. load_elf_binary() maps ET_EXEC segments with
 * MAP_FIXED_NOREPLACE; the second mmap of a shared page returns -EEXIST, and
 * since that happens after the point of no return the kernel can only SIGSEGV
 * the process. It presents as a bare "Segmentation fault" with no output and a
 * PC that is already unmapped.
 *
 * bfd ld merged these into a single RWE segment, which is why UML never had to
 * think about it and why this only shows up when linking with lld -- as the
 * Android NDK does. Asking for one segment explicitly reproduces the layout
 * that has always worked, rather than page-aligning a dozen boundaries that
 * come from macros and will drift.
 */
PHDRS {
  all PT_LOAD FILEHDR PHDRS FLAGS(7);   /* RWX, and map the ELF headers too */
  note PT_NOTE FLAGS(4);
  tls PT_TLS FLAGS(4);
}

SECTIONS
{
  /* This must contain the right address - not quite the default ELF one.*/
  PROVIDE (__executable_start = START);
  /* Static binaries stick stuff here, like the sigreturn trampoline,
   * invisibly to objdump.  So, just make __binary_start equal to the very
   * beginning of the executable, and if there are unmapped pages after this,
   * they are forever unusable.
   */
  __binary_start = START;

  . = START + SIZEOF_HEADERS;
  . = ALIGN(PAGE_SIZE);

  _text = .;
  INIT_TEXT_SECTION(0) :all

  .text      :
  {
    _stext = .;
    TEXT_TEXT
    SCHED_TEXT
    LOCK_TEXT
    IRQENTRY_TEXT
    SOFTIRQENTRY_TEXT
    *(.fixup)
    /* .gnu.warning sections are handled specially by elf32.em.  */
    *(.gnu.warning)
    *(.gnu.linkonce.t*)
  }

  . = ALIGN(PAGE_SIZE);
  .syscall_stub : {
	__syscall_stub_start = .;
	*(.__syscall_stub*)
	__syscall_stub_end = .;
  }

  /*
   * These are needed even in a static link, even if they wind up being empty.
   * Newer glibc needs these __rel{,a}_iplt_{start,end} symbols.
   */
  .rel.plt : {
	*(.rel.plt)
	PROVIDE_HIDDEN(__rel_iplt_start = .);
	*(.rel.iplt)
	PROVIDE_HIDDEN(__rel_iplt_end = .);
  }
  .rela.plt : {
	*(.rela.plt)
	PROVIDE_HIDDEN(__rela_iplt_start = .);
	*(.rela.iplt)
	PROVIDE_HIDDEN(__rela_iplt_end = .);
  }

  #include <asm/common.lds.S>

  __init_begin = .;
  init.data : { INIT_DATA }
  __init_end = .;

  /*
   * The C runtime's constructor arrays, placed exactly as dyn.lds.S places
   * them. This script -- the static one -- omitted them entirely, leaving the
   * linker to put them wherever its default rules chose, which lands them in a
   * segment of their own whose file offset does not match its virtual address.
   *
   * glibc tolerated that. bionic does not: __libc_init walks preinit_array
   * before anything else runs, so a statically linked UML against bionic
   * segfaults before main() with no output at all. The section was there in
   * the ELF -- correctly sized, at a plausible address -- which is what makes
   * this one hard to see.
   *
   * The alignment comment below is inherited from dyn.lds.S and is the reason
   * the label sits outside the section.
   */
  /*
   * Page-align every point where the segment permissions change.
   *
   * lld gives each permission group its own PT_LOAD, and takes the addresses
   * from this script -- so a group that starts mid-page produces two segments
   * sharing one page. load_elf_binary() maps ET_EXEC segments with
   * MAP_FIXED_NOREPLACE, the second mmap of that shared page returns -EEXIST,
   * and because this happens after the point of no return the kernel can only
   * SIGSEGV the process. The symptom is a bare "Segmentation fault" with no
   * output at all and a PC that is no longer mapped, which says nothing about
   * the cause.
   *
   * bfd ld merged these groups into one RWE segment and the question never
   * arose, which is why this only appears when linking with lld against the
   * Android NDK.
   */
  . = ALIGN(PAGE_SIZE);
  .preinit_array  : { *(.preinit_array) }
  .init_array     : {
    *(.kasan_init)
    *(.init_array.*)
    *(.init_array)
  }
  .fini_array     : { *(.fini_array) }

  .data    :
  {
    INIT_TASK_DATA(KERNEL_STACK_SIZE)
    DATA_DATA
    *(.gnu.linkonce.d*)
    CONSTRUCTORS
  }
  .data1   : { *(.data1) }
  .ctors         :
  {
    *(.ctors)
  }
  .dtors         :
  {
    *(.dtors)
  }

  .got           : { *(.got.plt) *(.got) }
  . = ALIGN(PAGE_SIZE);
  .eh_frame       : { KEEP (*(.eh_frame)) }
  .dynamic       : { *(.dynamic) }
  . = ALIGN(PAGE_SIZE);
  .tdata	  : { *(.tdata .tdata.* .gnu.linkonce.td.*) } :all :tls
  .tbss		  : { *(.tbss .tbss.* .gnu.linkonce.tb.*) *(.tcommon) } :all :tls
  /* We want the small data sections together, so single-instruction offsets
     can access them all, and initialized data all before uninitialized, so
     we can shorten the on-disk segment size.  */
  .sdata     : { *(.sdata) } :all
  _edata  =  .;
  PROVIDE (edata = .);
  . = ALIGN(PAGE_SIZE);
  __bss_start = .;
  PROVIDE(_bss_start = .);
  SBSS(0)
  BSS(0)
   __bss_stop = .;
  _end = .;
  PROVIDE (end = .);

  STABS_DEBUG
  DWARF_DEBUG
  MODINFO
  ELF_DETAILS

  DISCARDS
}
