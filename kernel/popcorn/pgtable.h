#ifndef __KERNEL_POPCORN_PGTABLE_H__
#define __KERNEL_POPCORN_PGTABLE_H__

#include <asm/pgtable.h>

#ifdef CONFIG_X86
static inline pte_t pte_make_invalid(pte_t entry)
{
	entry = pte_modify(entry, __pgprot(pte_flags(entry) & ~_PAGE_PRESENT));

	return entry;
}

static inline pte_t pte_make_valid(pte_t entry)
{
	entry = pte_modify(entry, __pgprot(pte_flags(entry) | _PAGE_PRESENT));

	return entry;
}

static inline bool pte_is_present(pte_t entry)
{
	return (pte_val(entry) & _PAGE_PRESENT) == _PAGE_PRESENT;
}

#else
#error "The architecture is not supported yet."
#endif

#endif
