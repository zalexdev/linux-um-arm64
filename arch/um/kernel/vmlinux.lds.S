#define RUNTIME_DISCARD_EXIT
/*
 * The guest kernel stack is measured in guest pages, not in 4K units: on a
 * 16K-page host CONFIG_KERNEL_STACK_ORDER=2 must mean four 16K pages, the same
 * as THREAD_SIZE in <asm/thread_info.h>, or the two disagree.
 */
KERNEL_STACK_SIZE = (1 << CONFIG_PAGE_SHIFT) * (1 << CONFIG_KERNEL_STACK_ORDER);

#ifdef CONFIG_LD_SCRIPT_STATIC
#include "uml.lds.S"
#else
#include "dyn.lds.S"
#endif
