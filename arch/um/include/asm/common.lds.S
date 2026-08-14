/* SPDX-License-Identifier: GPL-2.0 */
#include <asm-generic/vmlinux.lds.h>

/*
 * Section alignment below is (1 << CONFIG_PAGE_SHIFT), the *guest* page size,
 * which for UML always equals the host's. These were literal 4096s, correct
 * only on a 4K-page host. On a 16K-page host (Android 15 and later ship 16K) a
 * 4096-byte alignment lets read-only and read-write data share a page, so
 * CONFIG_STRICT_KERNEL_RWX either fails to mprotect or ends up granting write
 * access to rodata.
 */

  .fini      : { *(.fini)    } =0x9090
  _etext = .;
  PROVIDE (etext = .);

  . = ALIGN((1 << CONFIG_PAGE_SHIFT));
  _sdata = .;
  PROVIDE (sdata = .);

  RO_DATA((1 << CONFIG_PAGE_SHIFT))

  .unprotected : { *(.unprotected) }
  . = ALIGN((1 << CONFIG_PAGE_SHIFT));
  PROVIDE (_unprotected_end = .);

  . = ALIGN((1 << CONFIG_PAGE_SHIFT));
  EXCEPTION_TABLE(0)

  BUG_TABLE

  .uml.setup.init : {
	__uml_setup_start = .;
	*(.uml.setup.init)
	__uml_setup_end = .;
  }
	
  .uml.help.init : {
	__uml_help_start = .;
	*(.uml.help.init)
	__uml_help_end = .;
  }
	
  .uml.postsetup.init : {
	__uml_postsetup_start = .;
	*(.uml.postsetup.init)
	__uml_postsetup_end = .;
  }
	
  .init.setup : {
	INIT_SETUP(0)
  }

  PERCPU_SECTION(32)
	
  .initcall.init : {
	INIT_CALLS
  }

  .con_initcall.init : {
	CON_INITCALL
  }

  .exitcall : {
	__exitcall_begin = .;
	*(.exitcall.exit)
	__exitcall_end = .;
  }

  .uml.exitcall : {
	__uml_exitcall_begin = .;
	*(.uml.exitcall.exit)
	__uml_exitcall_end = .;
  }

  . = ALIGN(4);
  .altinstructions : {
	__alt_instructions = .;
	*(.altinstructions)
	__alt_instructions_end = .;
  }
  .altinstr_replacement : { *(.altinstr_replacement) }
  /* .exit.text is discard at runtime, not link time, to deal with references
     from .altinstructions and .eh_frame */
  .exit.text : { EXIT_TEXT }
  .exit.data : { *(.exit.data) }

  .preinit_array : {
	__preinit_array_start = .;
	*(.preinit_array)
	__preinit_array_end = .;
  }
  .init_array : {
	__init_array_start = .;
	*(.kasan_init)
	*(.init_array.*)
	*(.init_array)
	__init_array_end = .;
  }
  .fini_array : {
	__fini_array_start = .;
	*(.fini_array)
	__fini_array_end = .;
  }

   . = ALIGN((1 << CONFIG_PAGE_SHIFT));
  .init.ramfs : {
	INIT_RAM_FS
  }

