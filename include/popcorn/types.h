/**
 * @file include/popcorn/types.h
 *
 * Define constant variables and define optional features
 *
 * @author Sang-Hoon Kim, SSRG Virginia Tech, 2017
 * @author Marina Sadini, SSRG Virginia Tech, 2013
 */

#ifndef __INCLUDE_POPCORN_TYPES_H__
#define __INCLUDE_POPCORN_TYPES_H__

enum VMA_OPERATION_CODES {
	VMA_OP_NOP = 0,
	VMA_OP_UNMAP,
	VMA_OP_PROTECT,
	VMA_OP_REMAP,
	VMA_OP_MAP,
	VMA_OP_BRK,
	VMA_OP_MADVISE,
	VMA_OP_SAVE = -70,
	VMA_OP_NOT_SAVE = -71,
};


/* Selective features */
#undef MIGRATE_FPU
#undef MIGRATION_PROFILE
#undef STATISTICS

#include <popcorn/regset.h>

#endif /* __INCLUDE_POPCORN_TYPES_H__ */
