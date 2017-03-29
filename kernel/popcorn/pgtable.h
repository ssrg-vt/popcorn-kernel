#ifndef __KERNEL_POPCORN_PGTABLE_H__
#define __KERNEL_POPCORN_PGTABLE_H__

#include <asm/pgtable.h>

#ifdef CONFIG_X86
static inline pte_t pte_make_invalid(pte_t entry)
{
	entry = pte_clear_flags(entry, _PAGE_PRESENT);

	return entry;
}

static inline pte_t pte_make_valid(pte_t entry)
{
	entry = pte_set_flags(entry, _PAGE_PRESENT);

	return entry;
}

#elif defined(CONFIG_ARM64)

static inline pte_t pte_make_invalid(pte_t entry)
{
	entry = clear_pte_bit(entry, PTE_VALID);

	return entry;
}

static inline pte_t pte_make_valid(pte_t entry)
{
	entry = set_pte_bit(entry, PTE_VALID);

	return entry;
}

#else
#error "The architecture is not supported yet."
#endif

#endif
