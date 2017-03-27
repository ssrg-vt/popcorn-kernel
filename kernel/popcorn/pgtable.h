#ifndef __KERNEL_POPCORN_PGTABLE_H__
#define __KERNEL_POPCORN_PGTABLE_H__

#include <asm/pgtable.h>

#ifdef CONFIG_X86
pte_t pte_make_invalid(pte_t entry)
{
	entry = pte_set_flags(entry, _PAGE_SOFTW1);
	entry = pte_clear_flags(entry, _PAGE_PRESENT);

	return entry;
}

pte_t pte_make_valid(pte_t entry)
{
	entry = pte_clear_flags(entry, _PAGE_SOFTW1);
	entry = pte_set_flags(entry, _PAGE_PRESENT);

	return entry;
}

bool pte_is_invalid(pte_t entry)
{
	return pte_flags(entry) & _PAGE_SOFTW1;
}

#elif defined(CONFIG_ARM64)

pte_t pte_make_invalid(pte_t entry)
{
	entry = clear_pte_bit(entry, PTE_VALID);
	entry = pte_mkspecial(entry);
	return entry;
}

pte_t pte_make_valid(pte_t entry)
{
	entry = set_pte_bit(entry, PTE_VALID);
	entry = clear_pte_bit(entry, PTE_SPECIAL);
	return entry;
}

bool pte_is_invalid(pte_t entry)
{
	return pte_val(entry) & PTE_VALID;
}

#else
#error "The architecture is not supported yet."
#endif

#endif
