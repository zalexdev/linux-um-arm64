/* SPDX-License-Identifier: GPL-2.0 */
/*
 * Copyright 2003 PathScale Inc
 * Derived from include/asm-i386/pgtable.h
 */

#ifndef __UM_PGTABLE_4LEVEL_H
#define __UM_PGTABLE_4LEVEL_H

#include <asm-generic/pgtable-nop4d.h>

/* PGDIR_SHIFT determines what a fourth-level page table entry can map */

#define PGDIR_SHIFT	39
#define PGDIR_SIZE	(1UL << PGDIR_SHIFT)
#define PGDIR_MASK	(~(PGDIR_SIZE-1))

/* PUD_SHIFT determines the size of the area a third-level page table can
 * map
 */

#define PUD_SHIFT	30
#define PUD_SIZE	(1UL << PUD_SHIFT)
#define PUD_MASK	(~(PUD_SIZE-1))

/* PMD_SHIFT determines the size of the area a second-level page table can
 * map
 */

#define PMD_SHIFT	21
#define PMD_SIZE	(1UL << PMD_SHIFT)
#define PMD_MASK	(~(PMD_SIZE-1))

/*
 * entries per page directory level
 */

/*
 * PTRS_PER_PTE is how many pages one PMD covers, so it follows PAGE_SHIFT and
 * cannot be a literal.
 *
 * At 4 KB pages this is 2 MB >> 12 = 512, exactly what the literal said. At
 * 16 KB it is 128, and the literal was four times too large -- with real
 * consequences, because generic mm reads PTRS_PER_PTE as "entries belonging to
 * this PMD". finish_fault() bounds a large folio's PTE run with it, so a run
 * four times too long was permitted and set_ptes() wrote past the window this
 * PMD actually owns, into slots no lookup for those addresses will ever
 * consult and zap_pte_range() will never free -- while the rmap and rss
 * counters had already counted them. The result is a guest that re-faults
 * forever on those addresses and prints "Bad rss-counter state" as each
 * process exits. Silent, not a crash: the stray index still lands inside the
 * page-table page.
 *
 * ext4 makes this reachable rather than theoretical: it enables large folios
 * on every regular file, and any folio of order 7 or above straddles a PMD at
 * 16 KB pages.
 *
 * The upper three levels stay literal because their invariants already hold at
 * both page sizes: 512 * PMD_SIZE == PUD_SIZE and so on up.
 */
#define PTRS_PER_PTE (1UL << (PMD_SHIFT - PAGE_SHIFT))
#define PTRS_PER_PMD 512
#define PTRS_PER_PUD 512
#define PTRS_PER_PGD 512

/* The invariant the literal used to break. */
static_assert(PTRS_PER_PTE == (PMD_SIZE >> PAGE_SHIFT));

#define USER_PTRS_PER_PGD ((TASK_SIZE + (PGDIR_SIZE - 1)) / PGDIR_SIZE)

#define pte_ERROR(e) \
        printk("%s:%d: bad pte %p(%016lx).\n", __FILE__, __LINE__, &(e), \
	       pte_val(e))
#define pmd_ERROR(e) \
        printk("%s:%d: bad pmd %p(%016lx).\n", __FILE__, __LINE__, &(e), \
	       pmd_val(e))
#define pud_ERROR(e) \
        printk("%s:%d: bad pud %p(%016lx).\n", __FILE__, __LINE__, &(e), \
	       pud_val(e))
#define pgd_ERROR(e) \
        printk("%s:%d: bad pgd %p(%016lx).\n", __FILE__, __LINE__, &(e), \
	       pgd_val(e))

#define pud_none(x)	(!(pud_val(x) & ~_PAGE_NEEDSYNC))
#define	pud_bad(x)	((pud_val(x) & (~PAGE_MASK & ~_PAGE_USER)) != _KERNPG_TABLE)
#define pud_present(x)	(pud_val(x) & _PAGE_PRESENT)
#define pud_populate(mm, pud, pmd) \
	set_pud(pud, __pud(_PAGE_TABLE + __pa(pmd)))

#define set_pud(pudptr, pudval) (*(pudptr) = (pudval))

#define p4d_none(x)	(!(p4d_val(x) & ~_PAGE_NEEDSYNC))
#define	p4d_bad(x)	((p4d_val(x) & (~PAGE_MASK & ~_PAGE_USER)) != _KERNPG_TABLE)
#define p4d_present(x)	(p4d_val(x) & _PAGE_PRESENT)
#define p4d_populate(mm, p4d, pud) \
	set_p4d(p4d, __p4d(_PAGE_TABLE + __pa(pud)))

#define set_p4d(p4dptr, p4dval) (*(p4dptr) = (p4dval))


static inline int pgd_needsync(pgd_t pgd)
{
	return pgd_val(pgd) & _PAGE_NEEDSYNC;
}

static inline void pgd_mkuptodate(pgd_t pgd) { pgd_val(pgd) &= ~_PAGE_NEEDSYNC; }

#define set_pmd(pmdptr, pmdval) (*(pmdptr) = (pmdval))

static inline void pud_clear (pud_t *pud)
{
	set_pud(pud, __pud(_PAGE_NEEDSYNC));
}

static inline void p4d_clear (p4d_t *p4d)
{
	set_p4d(p4d, __p4d(_PAGE_NEEDSYNC));
}

#define pud_page(pud) phys_to_page(pud_val(pud) & PAGE_MASK)
#define pud_pgtable(pud) ((pmd_t *) __va(pud_val(pud) & PAGE_MASK))

#define p4d_page(p4d) phys_to_page(p4d_val(p4d) & PAGE_MASK)
#define p4d_pgtable(p4d) ((pud_t *) __va(p4d_val(p4d) & PAGE_MASK))

static inline unsigned long pte_pfn(pte_t pte)
{
	return phys_to_pfn(pte_val(pte));
}

static inline pmd_t pfn_pmd(unsigned long page_nr, pgprot_t pgprot)
{
	return __pmd((page_nr << PAGE_SHIFT) | pgprot_val(pgprot));
}

#endif
