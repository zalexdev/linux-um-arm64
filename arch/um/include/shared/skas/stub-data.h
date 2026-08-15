/* SPDX-License-Identifier: GPL-2.0 */
/*

 * Copyright (C) 2015 Thomas Meyer (thomas@m3y3r.de)
 * Copyright (C) 2005 Jeff Dike (jdike@karaya.com)
 */

#ifndef __STUB_DATA_H
#define __STUB_DATA_H

#include <linux/compiler_types.h>
#include <as-layout.h>
#include <sysdep/tls.h>
#include <sysdep/stub-data.h>
#include <mm_id.h>

/* See the note in skas/mm_id.h: a sysroot's linux/compiler_types.h can shadow
 * the kernel's, leaving the attribute macros undefined for USER_CFLAGS objects.
 */
#ifndef __aligned
#define __aligned(x)	__attribute__((__aligned__(x)))
#endif
#ifndef __section
#define __section(x)	__attribute__((__section__(x)))
#endif
#ifndef __used
#define __used		__attribute__((__used__))
#endif

#define FUTEX_IN_CHILD 0
#define FUTEX_IN_KERN 1

struct stub_init_data {
	int seccomp;

	unsigned long stub_start;

	int stub_code_fd;
	unsigned long stub_code_offset;
	int stub_data_fd;
	unsigned long stub_data_offset;

	unsigned long signal_handler;
	unsigned long signal_restorer;
};

#define STUB_NEXT_SYSCALL(s) \
	((struct stub_syscall *) (((unsigned long) s) + (s)->cmd_len))

enum stub_syscall_type {
	STUB_SYSCALL_UNSET = 0,
	STUB_SYSCALL_MMAP,
	STUB_SYSCALL_MUNMAP,
};

struct stub_syscall {
	struct {
		unsigned long addr;
		unsigned long length;
		unsigned long offset;
		int fd;
		int prot;
	} mem;

	enum stub_syscall_type syscall;
};

struct stub_data {
	long err;

	int syscall_data_len;
	/* 128 leaves enough room for additional fields in the struct */
	struct stub_syscall syscall_data[(UM_KERN_PAGE_SIZE - 128) / sizeof(struct stub_syscall)] __aligned(16);

	/* data shared with signal handler (only used in seccomp mode) */
	short restart_wait;
	unsigned int futex;
	int signal;
	unsigned short si_offset;
	unsigned short mctx_offset;

	/* seccomp architecture specific state restore */
	struct stub_data_arch arch_data;

	/*
	 * Stack for our signal handlers and for calling into .
	 *
	 * Sized from STUB_DATA_PAGES rather than fixed at one page: in SECCOMP
	 * mode the host writes a whole signal frame here, and on arm64 that
	 * frame is larger than a 4 KiB page. See the comment on STUB_DATA_PAGES
	 * in as-layout.h.
	 */
	unsigned char sigstack[(STUB_DATA_PAGES - 1) * UM_KERN_PAGE_SIZE]
		__aligned(UM_KERN_PAGE_SIZE);
};

#endif
