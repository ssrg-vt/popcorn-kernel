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

#elif defined(CONFIG_ARM64)
static inline unsigned long pte_flags(pte_t entry)
{
        return pte_val(entry);
}

static inline pte_t pte_make_invalid(pte_t entry)
{
	entry = clear_pte_bit(entry, __pgprot(PTE_VALID));

	return entry;
}

static inline pte_t pte_make_valid(pte_t entry)
{
	entry = set_pte_bit(entry, __pgprot(PTE_VALID));

	return entry;
}

static inline bool pte_is_present(pte_t entry)
{
	return (pte_val(entry) & PTE_VALID) == PTE_VALID;
}

#elif defined(CONFIG_RISCV)
/* CJP: FIXME  */
static inline unsigned long pte_flags(pte_t entry)
{
        return pte_val(entry);
}

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
