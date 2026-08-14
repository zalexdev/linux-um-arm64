// SPDX-License-Identifier: GPL-2.0
/*
 * x86 subarch half of UML's /proc/cpuinfo.
 *
 * Moved verbatim out of arch/um/kernel/um_arch.c when the CPU-feature model was
 * made subarch-specific. Behaviour is unchanged: the guest mirrors the host's
 * CPUID feature names so that guest userspace which greps /proc/cpuinfo for a
 * flag sees exactly what it would see on the host.
 */
#include <linux/seq_file.h>
#include <linux/string.h>
#include <linux/string_choices.h>

#include <asm/cpufeature.h>
#include <asm/processor.h>
#include <arch.h>

void arch_show_cpuinfo(struct seq_file *m)
{
	int i;

	seq_printf(m, "fpu\t\t: %s\n",
		   str_yes_no(cpu_has(&boot_cpu_data, X86_FEATURE_FPU)));
	seq_printf(m, "flags\t\t:");
	for (i = 0; i < 32 * NCAPINTS; i++)
		if (cpu_has(&boot_cpu_data, i) && (x86_cap_flags[i] != NULL))
			seq_printf(m, " %s", x86_cap_flags[i]);
	seq_printf(m, "\n");
}

int arch_parse_host_cpu_flags(char *line)
{
	int i;

	if (!strstr(line, "flags"))
		return 0;

	for (i = 0; i < 32 * NCAPINTS; i++) {
		if ((x86_cap_flags[i] != NULL) && strstr(line, x86_cap_flags[i]))
			set_cpu_cap(&boot_cpu_data, i);
	}

	/*
	 * cache_alignment may still be ahead of us in the file, so this is not
	 * yet a reason to stop reading.
	 */
	return 0;
}
