/* SPDX-License-Identifier: GPL-2.0 */
#ifndef __UM_SHARED_SMP_H
#define __UM_SHARED_SMP_H

#if IS_ENABLED(CONFIG_SMP)

extern int uml_ncpus;
void um_ncpus_init(void);

/*
 * Drop back to one CPU. A setter rather than a plain assignment because
 * uml_ncpus is a constant on a uniprocessor build, where this is a no-op --
 * the alternative is an #ifdef at every caller, which is how the ptrace path
 * came to break the !SMP build.
 */
static inline void um_ncpus_force_up(void) { uml_ncpus = 1; }

int uml_curr_cpu(void);
void uml_start_secondary(void *opaque);
void uml_ipi_handler(int vector);

#else /* !CONFIG_SMP */

#define uml_ncpus 1
#define uml_curr_cpu() 0
static inline void um_ncpus_init(void) { }
static inline void um_ncpus_force_up(void) { }

#endif /* CONFIG_SMP */

#endif /* __UM_SHARED_SMP_H */
