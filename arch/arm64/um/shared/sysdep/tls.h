/* SPDX-License-Identifier: GPL-2.0 */
#ifndef _SYSDEP_TLS_H
#define _SYSDEP_TLS_H

/*
 * arm64 has no segment descriptors and no set_thread_area(2): thread-local
 * storage is just the TPIDR_EL0 system register, which EL0 reads and writes
 * itself. This header exists only because arch/um/include/shared/skas/stub-data.h
 * includes <sysdep/tls.h> unconditionally, and because x86 needs a
 * user_desc-shaped type there.
 *
 * user_desc_t is defined as an empty-ish placeholder rather than omitted so
 * that generic code which merely mentions the type still compiles; nothing on
 * arm64 ever populates one.
 */

typedef struct um_arm64_tls_desc {
	unsigned long tp_value;		/* TPIDR_EL0 */
} user_desc_t;

#endif /* _SYSDEP_TLS_H */
