// SPDX-License-Identifier: GPL-2.0
/*
 * Copyright (C) 2000 - 2007 Jeff Dike (jdike@{addtoit,linux.intel}.com)
 */

#include <linux/mm.h>
#include <linux/sched/signal.h>
#include <linux/hardirq.h>
#include <linux/module.h>
#include <linux/sizes.h>
#include <linux/uaccess.h>
#include <linux/userfaultfd_k.h>
#include <linux/sched/debug.h>
#include <asm/current.h>
#include <asm/tlbflush.h>
#include <arch.h>
#include <as-layout.h>
#include <kern_util.h>
#include <os.h>
#include <skas.h>

/*
 * NOTE: UML does not have exception tables. As such, this is almost a copy
 * of the code in mm/memory.c, only adjusting the logic to simply check whether
 * we are coming from the kernel instead of doing an additional lookup in the
 * exception table.
 * We can do this simplification because we never get here if the exception was
 * fixable.
 */
static inline bool get_mmap_lock_carefully(struct mm_struct *mm, bool is_user)
{
	if (likely(mmap_read_trylock(mm)))
		return true;

	if (!is_user)
		return false;

	return !mmap_read_lock_killable(mm);
}

static inline bool mmap_upgrade_trylock(struct mm_struct *mm)
{
	/*
	 * We don't have this operation yet.
	 *
	 * It should be easy enough to do: it's basically a
	 *    atomic_long_try_cmpxchg_acquire()
	 * from RWSEM_READER_BIAS -> RWSEM_WRITER_LOCKED, but
	 * it also needs the proper lockdep magic etc.
	 */
	return false;
}

static inline bool upgrade_mmap_lock_carefully(struct mm_struct *mm, bool is_user)
{
	mmap_read_unlock(mm);
	if (!is_user)
		return false;

	return !mmap_write_lock_killable(mm);
}

/*
 * Helper for page fault handling.
 *
 * This is kind of equivalend to "mmap_read_lock()" followed
 * by "find_extend_vma()", except it's a lot more careful about
 * the locking (and will drop the lock on failure).
 *
 * For example, if we have a kernel bug that causes a page
 * fault, we don't want to just use mmap_read_lock() to get
 * the mm lock, because that would deadlock if the bug were
 * to happen while we're holding the mm lock for writing.
 *
 * So this checks the exception tables on kernel faults in
 * order to only do this all for instructions that are actually
 * expected to fault.
 *
 * We can also actually take the mm lock for writing if we
 * need to extend the vma, which helps the VM layer a lot.
 */
static struct vm_area_struct *
um_lock_mm_and_find_vma(struct mm_struct *mm,
			unsigned long addr, bool is_user)
{
	struct vm_area_struct *vma;

	if (!get_mmap_lock_carefully(mm, is_user))
		return NULL;

	vma = find_vma(mm, addr);
	if (likely(vma && (vma->vm_start <= addr)))
		return vma;

	/*
	 * Well, dang. We might still be successful, but only
	 * if we can extend a vma to do so.
	 */
	if (!vma || !(vma->vm_flags & VM_GROWSDOWN)) {
		mmap_read_unlock(mm);
		return NULL;
	}

	/*
	 * We can try to upgrade the mmap lock atomically,
	 * in which case we can continue to use the vma
	 * we already looked up.
	 *
	 * Otherwise we'll have to drop the mmap lock and
	 * re-take it, and also look up the vma again,
	 * re-checking it.
	 */
	if (!mmap_upgrade_trylock(mm)) {
		if (!upgrade_mmap_lock_carefully(mm, is_user))
			return NULL;

		vma = find_vma(mm, addr);
		if (!vma)
			goto fail;
		if (vma->vm_start <= addr)
			goto success;
		if (!(vma->vm_flags & VM_GROWSDOWN))
			goto fail;
	}

	if (expand_stack_locked(vma, addr))
		goto fail;

success:
	mmap_write_downgrade(mm);
	return vma;

fail:
	mmap_write_unlock(mm);
	return NULL;
}

/*
 * Anonymous fault-around.
 *
 * Every guest page fault costs a full stub handoff: SIGSEGV in the child,
 * futex/ptrace transfer to the UML kernel thread, and a second transfer back
 * to resume -- 18.9us (seccomp) / 41.0us (ptrace) per fault on the reference
 * device, dominated entirely by the two voluntary context switches, not by
 * handle_mm_fault. The stub mmap transport, however, is already a
 * batch: um_tlb_sync walks the marked PTE range and queues one stub_syscall
 * per page, and the whole queue is executed in the *same* handoff that
 * resumes the guest (see userspace() in os-Linux/skas/process.c). So if we
 * install K extra PTEs while we are here anyway, K pages ride one handoff
 * instead of costing K.
 *
 * This helps only paths where each page really would fault separately, i.e.
 * the guest touching fresh anonymous memory. It does NOT help copy_to_user
 * and friends: uaccess runs in the kernel address space and takes no per-page
 * stub handoff in the first place.
 *
 * The speculation is restricted to vma_is_anonymous() && pte_none() &&
 * !userfaultfd_armed():
 *  - file-backed faults can do I/O and have their own fault-around upstream;
 *  - a non-none PTE may be a swap entry, and do_swap_page can block or
 *    return VM_FAULT_RETRY -- pte_none keeps us on the do_anonymous_page
 *    path only;
 *  - userfaultfd must see real faults, not speculative ones.
 *
 * The window ramps 0 -> 4 -> 16 -> 32 pages driven by a sequential-stream
 * detector, so a random-access workload never pays for pages it will not
 * touch; only a stream that keeps faulting exactly past the previous window
 * earns a bigger one. The size cap is expressed in BYTES ("prefault="
 * command-line option, default 128K, 0 disables) because PAGE_SIZE is
 * parametric here: with the page-count cap a 16K-page guest would silently
 * speculate a 512K window.
 */
static const unsigned int um_prefault_ramp[] = {0, 4, 16, 32};
#define UM_PREFAULT_LEVEL_MAX (ARRAY_SIZE(um_prefault_ramp) - 1)

static unsigned long um_prefault_bytes __read_mostly = SZ_128K;

static int __init um_prefault_setup(char *str)
{
	um_prefault_bytes = memparse(str, &str);
	return 1;
}
__setup("prefault=", um_prefault_setup);

/*
 * Count the leading run of pte_none() pages in [addr, seg_end), where the
 * caller guarantees the range does not cross a PMD (or folded-level)
 * boundary, so a single PTE table covers it. Sets *stop if the scan hit
 * something that ends the speculation window (a populated PTE, a swap entry,
 * or a PTE table we cannot map).
 */
static unsigned int um_prefault_scan(struct mm_struct *mm, unsigned long addr,
				     unsigned long seg_end, bool *stop)
{
	unsigned int max = (seg_end - addr) >> PAGE_SHIFT;
	unsigned int nr = 0;
	pgd_t *pgd;
	p4d_t *p4d;
	pud_t *pud;
	pmd_t *pmd;
	pte_t *pte;

	/*
	 * A missing table at any level means every PTE below it is none, and
	 * that is the common case for a fresh allocation: the whole segment
	 * is prefaultable and handle_mm_fault will build the tables.
	 */
	pgd = pgd_offset(mm, addr);
	if (pgd_none(*pgd))
		return max;
	if (unlikely(pgd_bad(*pgd)))
		goto stop;

	p4d = p4d_offset(pgd, addr);
	if (p4d_none(*p4d))
		return max;
	if (unlikely(p4d_bad(*p4d)))
		goto stop;

	pud = pud_offset(p4d, addr);
	if (pud_none(*pud))
		return max;
	if (unlikely(pud_bad(*pud)))
		goto stop;

	pmd = pmd_offset(pud, addr);
	if (pmd_none(*pmd))
		return max;
	if (unlikely(pmd_bad(*pmd)))
		goto stop;

	/*
	 * pte_offset_map() may return NULL (racing table teardown). We are
	 * purely speculative, so any ambiguity just ends the window.
	 */
	pte = pte_offset_map(pmd, addr);
	if (!pte)
		goto stop;

	while (nr < max && pte_none(pte[nr]))
		nr++;
	pte_unmap(pte);

	if (nr < max)
		*stop = true;
	return nr;

stop:
	*stop = true;
	return 0;
}

/*
 * Called with mmap_read_lock held, right after the primary fault succeeded
 * and before the single handoff that will publish it to the stub.
 *
 * Returns false only if handle_mm_fault dropped the mmap lock (the caller
 * must then skip its unlock). With the flags we pass that is a can't-happen,
 * but see the comment at the VM_FAULT_RETRY check -- silently guessing wrong
 * about lock ownership here is either a double-unlock or a leaked lock.
 */
static bool um_prefault_around(struct mm_struct *mm, struct vm_area_struct *vma,
			       unsigned long address, unsigned int flags)
{
	unsigned long addr, end;
	unsigned int level = 0, nr;
	int slots;

	if (!um_prefault_bytes)
		return true;

	if (!vma_is_anonymous(vma) || userfaultfd_armed(vma))
		return true;

	address &= PAGE_MASK;

	/*
	 * Sequential-stream detector: a stream that consumed its previous
	 * window faults exactly at prefault_next. Anything else resets the
	 * ramp, so the first fault of any stream (and every fault of a random
	 * workload) prefaults nothing.
	 */
	if (address == READ_ONCE(mm->context.prefault_next)) {
		level = READ_ONCE(mm->context.prefault_level);
		level = min_t(unsigned int, level + 1, UM_PREFAULT_LEVEL_MAX);
	}
	WRITE_ONCE(mm->context.prefault_level, level);

	nr = um_prefault_ramp[level];
	nr = min_t(unsigned long, nr, um_prefault_bytes >> PAGE_SHIFT);

	/*
	 * Anonymous pages land at arbitrary physmem offsets, so map() almost
	 * never merges them: budget one syscall_data slot per page, plus one
	 * for the primary fault, and never overflow the batch -- an overflow
	 * flushes mid-batch, which is exactly the extra handoff this code
	 * exists to avoid.
	 */
	slots = syscall_stub_free_slots(&mm->context.id) - 1;
	if (slots < 0)
		slots = 0;
	nr = min_t(unsigned int, nr, slots);

	addr = address + PAGE_SIZE;
	end = addr + (unsigned long)nr * PAGE_SIZE;
	if (end > vma->vm_end)
		end = vma->vm_end;
	if (end < addr)
		end = addr;

	/* Next fault of a sequential stream lands one page past the window. */
	WRITE_ONCE(mm->context.prefault_next, end);

	/*
	 * FAULT_FLAG_ALLOW_RETRY must go: VM_FAULT_RETRY releases the mmap
	 * lock, and our caller still owns a read lock it will unlock exactly
	 * once. KILLABLE/INTERRUPTIBLE must go with it -- they are only valid
	 * with retry, and a speculative fault has no business aborting on a
	 * signal that the (already-satisfied) primary fault will handle.
	 */
	flags &= ~(FAULT_FLAG_ALLOW_RETRY | FAULT_FLAG_KILLABLE |
		   FAULT_FLAG_INTERRUPTIBLE | FAULT_FLAG_TRIED);

	while (addr < end) {
		unsigned long seg_end;
		unsigned int i, pages;
		bool stop = false;

		/*
		 * Clamp the scan to one PTE table. Cascading through every
		 * level's addr_end keeps this correct whichever levels are
		 * folded (2-level and 4-level layouts both build here).
		 */
		seg_end = pgd_addr_end(addr, end);
		seg_end = p4d_addr_end(addr, seg_end);
		seg_end = pud_addr_end(addr, seg_end);
		seg_end = pmd_addr_end(addr, seg_end);

		pages = um_prefault_scan(mm, addr, seg_end, &stop);

		for (i = 0; i < pages; i++, addr += PAGE_SIZE) {
			vm_fault_t fault;

			fault = handle_mm_fault(vma, addr, flags, NULL);

			/*
			 * Cannot happen for a pte_none anonymous fault with
			 * ALLOW_RETRY stripped; if the mm core ever breaks
			 * that contract, both codes mean it dropped the mmap
			 * lock, so tell the caller instead of double-
			 * unlocking.
			 */
			if (WARN_ON_ONCE(fault &
					 (VM_FAULT_RETRY | VM_FAULT_COMPLETED)))
				return false;

			/*
			 * Speculative failure (e.g. OOM) must not surface:
			 * the primary fault already succeeded, the guest will
			 * come back for this page if it really wants it.
			 */
			if (fault & VM_FAULT_ERROR)
				return true;
		}

		if (stop)
			break;
		addr = seg_end;
	}

	return true;
}

/*
 * Note this is constrained to return 0, -EFAULT, -EACCES, -ENOMEM by
 * segv().
 */
int handle_page_fault(unsigned long address, unsigned long ip,
		      int is_write, int is_user, int *code_out)
{
	struct mm_struct *mm = current->mm;
	struct vm_area_struct *vma;
	pmd_t *pmd;
	pte_t *pte;
	int err = -EFAULT;
	unsigned int flags = FAULT_FLAG_DEFAULT;

	*code_out = SEGV_MAPERR;

	/*
	 * If the fault was with pagefaults disabled, don't take the fault, just
	 * fail.
	 */
	if (faulthandler_disabled())
		goto out_nosemaphore;

	if (is_user)
		flags |= FAULT_FLAG_USER;
retry:
	vma = um_lock_mm_and_find_vma(mm, address, is_user);
	if (!vma)
		goto out_nosemaphore;

	*code_out = SEGV_ACCERR;
	if (is_write) {
		if (!(vma->vm_flags & VM_WRITE))
			goto out;
		flags |= FAULT_FLAG_WRITE;
	} else {
		/* Don't require VM_READ|VM_EXEC for write faults! */
		if (!(vma->vm_flags & (VM_READ | VM_EXEC)))
			goto out;
	}

	do {
		vm_fault_t fault;

		fault = handle_mm_fault(vma, address, flags, NULL);

		if ((fault & VM_FAULT_RETRY) && fatal_signal_pending(current))
			goto out_nosemaphore;

		/* The fault is fully completed (including releasing mmap lock) */
		if (fault & VM_FAULT_COMPLETED)
			return 0;

		if (unlikely(fault & VM_FAULT_ERROR)) {
			if (fault & VM_FAULT_OOM) {
				goto out_of_memory;
			} else if (fault & VM_FAULT_SIGSEGV) {
				goto out;
			} else if (fault & VM_FAULT_SIGBUS) {
				err = -EACCES;
				goto out;
			}
			BUG();
		}
		if (fault & VM_FAULT_RETRY) {
			flags |= FAULT_FLAG_TRIED;

			goto retry;
		}

		pmd = pmd_off(mm, address);
		pte = pte_offset_kernel(pmd, address);
	} while (!pte_present(*pte));
	err = 0;

	/*
	 * The fault is resolved but not yet published to the stub -- that
	 * happens in one batched handoff on the way back to userspace. Piggy-
	 * back a fault-around window onto that same handoff. Guest faults
	 * only: a kernel-mode fault (uaccess) pays no per-page handoff, so
	 * there is nothing to save.
	 */
	if (is_user && !um_prefault_around(mm, vma, address, flags))
		goto out_nosemaphore;

	/*
	 * The below warning was added in place of
	 *	pte_mkyoung(); if (is_write) pte_mkdirty();
	 * If it's triggered, we'd see normally a hang here (a clean pte is
	 * marked read-only to emulate the dirty bit).
	 * However, the generic code can mark a PTE writable but clean on a
	 * concurrent read fault, triggering this harmlessly. So comment it out.
	 */
#if 0
	WARN_ON(!pte_young(*pte) || (is_write && !pte_dirty(*pte)));
#endif

out:
	mmap_read_unlock(mm);
out_nosemaphore:
	return err;

out_of_memory:
	/*
	 * We ran out of memory, call the OOM killer, and return the userspace
	 * (which will retry the fault, or kill us if we got oom-killed).
	 */
	mmap_read_unlock(mm);
	if (!is_user)
		goto out_nosemaphore;
	pagefault_out_of_memory();
	return 0;
}

static void show_segv_info(struct uml_pt_regs *regs)
{
	struct task_struct *tsk = current;
	struct faultinfo *fi = UPT_FAULTINFO(regs);

	if (!unhandled_signal(tsk, SIGSEGV))
		return;

	if (!printk_ratelimit())
		return;

	printk("%s%s[%d]: segfault at %lx ip %px sp %px error %x",
		task_pid_nr(tsk) > 1 ? KERN_INFO : KERN_EMERG,
		tsk->comm, task_pid_nr(tsk), FAULT_ADDRESS(*fi),
		(void *)UPT_IP(regs), (void *)UPT_SP(regs),
		FAULT_ERROR_CODE(*fi));

	print_vma_addr(KERN_CONT " in ", UPT_IP(regs));
	printk(KERN_CONT "\n");
}

static void bad_segv(struct faultinfo fi, unsigned long ip)
{
	current->thread.arch.faultinfo = fi;
	force_sig_fault(SIGSEGV, SEGV_ACCERR, (void __user *) FAULT_ADDRESS(fi));
}

void fatal_sigsegv(void)
{
	force_fatal_sig(SIGSEGV);
	do_signal(&current->thread.regs);
	/*
	 * This is to tell gcc that we're not returning - do_signal
	 * can, in general, return, but in this case, it's not, since
	 * we just got a fatal SIGSEGV queued.
	 */
	os_dump_core();
}

/**
 * segv_handler() - the SIGSEGV handler
 * @sig:	the signal number
 * @unused_si:	the signal info struct; unused in this handler
 * @regs:	the ptrace register information
 * @mc:		the mcontext of the signal
 *
 * The handler first extracts the faultinfo from the UML ptrace regs struct.
 * If the userfault did not happen in an UML userspace process, bad_segv is called.
 * Otherwise the signal did happen in a cloned userspace process, handle it.
 */
void segv_handler(int sig, struct siginfo *unused_si, struct uml_pt_regs *regs,
		  void *mc)
{
	struct faultinfo * fi = UPT_FAULTINFO(regs);

	if (UPT_IS_USER(regs) && !SEGV_IS_FIXABLE(fi)) {
		show_segv_info(regs);
		bad_segv(*fi, UPT_IP(regs));
		return;
	}
	segv(*fi, UPT_IP(regs), UPT_IS_USER(regs), regs, mc);
}

/*
 * We give a *copy* of the faultinfo in the regs to segv.
 * This must be done, since nesting SEGVs could overwrite
 * the info in the regs. A pointer to the info then would
 * give us bad data!
 */
unsigned long segv(struct faultinfo fi, unsigned long ip, int is_user,
		   struct uml_pt_regs *regs, void *mc)
{
	int si_code;
	int err;
	int is_write = FAULT_WRITE(fi);
	unsigned long address = FAULT_ADDRESS(fi);

	if (!is_user && regs)
		current->thread.segv_regs = container_of(regs, struct pt_regs, regs);

	if (!is_user && address >= start_vm && address < end_vm) {
		/*
		 * Kernel has pending updates from set_ptes that were not
		 * flushed yet. Syncing them should fix the pagefault (if not
		 * we'll get here again and panic).
		 */
		err = um_tlb_sync(&init_mm);
		if (err == -ENOMEM)
			report_enomem();
		if (err)
			panic("Failed to sync kernel TLBs: %d", err);
		goto out;
	}
	else if (current->pagefault_disabled) {
		if (!mc) {
			show_regs(container_of(regs, struct pt_regs, regs));
			panic("Segfault with pagefaults disabled but no mcontext: addr 0x%lx, ip 0x%lx, %s",
			      address, ip, is_write ? "write" : "read");
		}
		if (!current->thread.segv_continue) {
			show_regs(container_of(regs, struct pt_regs, regs));
			panic("Segfault without recovery target: addr 0x%lx, ip 0x%lx, %s",
			      address, ip, is_write ? "write" : "read");
		}
		mc_set_rip(mc, current->thread.segv_continue);
		current->thread.segv_continue = NULL;
		goto out;
	}
	else if (current->mm == NULL) {
		show_regs(container_of(regs, struct pt_regs, regs));
		panic("Segfault with no mm: addr 0x%lx, ip 0x%lx, %s",
			      address, ip, is_write ? "write" : "read");
	}
	else if (!is_user && address > PAGE_SIZE && address < TASK_SIZE) {
		show_regs(container_of(regs, struct pt_regs, regs));
		panic("Kernel tried to access user memory at addr 0x%lx, ip 0x%lx",
		       address, ip);
	}

	if (SEGV_IS_FIXABLE(&fi))
		err = handle_page_fault(address, ip, is_write, is_user,
					&si_code);
	else {
		err = -EFAULT;
		/*
		 * A thread accessed NULL, we get a fault, but CR2 is invalid.
		 * This code is used in __do_copy_from_user() of TT mode.
		 * XXX tt mode is gone, so maybe this isn't needed any more
		 */
		address = 0;
	}

	if (!err)
		goto out;
	else if (!is_user && arch_fixup(ip, regs))
		goto out;

	if (!is_user) {
		show_regs(container_of(regs, struct pt_regs, regs));
		panic("Kernel mode fault at addr 0x%lx, ip 0x%lx",
		      address, ip);
	}

	show_segv_info(regs);

	if (err == -EACCES) {
		current->thread.arch.faultinfo = fi;
		force_sig_fault(SIGBUS, BUS_ADRERR, (void __user *)address);
	} else {
		BUG_ON(err != -EFAULT);
		current->thread.arch.faultinfo = fi;
		force_sig_fault(SIGSEGV, si_code, (void __user *) address);
	}

out:
	if (regs)
		current->thread.segv_regs = NULL;

	return 0;
}

void relay_signal(int sig, struct siginfo *si, struct uml_pt_regs *regs,
		  void *mc)
{
	int code, err;
	if (!UPT_IS_USER(regs)) {
		if (sig == SIGBUS)
			printk(KERN_ERR "Bus error - the host /dev/shm or /tmp "
			       "mount likely just ran out of space\n");
		panic("Kernel mode signal %d", sig);
	}

	arch_examine_signal(sig, regs);

	/* Is the signal layout for the signal known?
	 * Signal data must be scrubbed to prevent information leaks.
	 */
	code = si->si_code;
	err = si->si_errno;
	if ((err == 0) && (siginfo_layout(sig, code) == SIL_FAULT)) {
		struct faultinfo *fi = UPT_FAULTINFO(regs);
		current->thread.arch.faultinfo = *fi;
		force_sig_fault(sig, code, (void __user *)FAULT_ADDRESS(*fi));
	} else {
		printk(KERN_ERR "Attempted to relay unknown signal %d (si_code = %d) with errno %d\n",
		       sig, code, err);
		force_sig(sig);
	}
}

void winch(int sig, struct siginfo *unused_si, struct uml_pt_regs *regs,
	   void *mc)
{
	do_IRQ(WINCH_IRQ, regs);
}
