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

enum REPLICATION_STATUS {
	REPLICATION_STATUS_NOT_REPLICATED = 0,
	REPLICATION_STATUS_WRITTEN,
	REPLICATION_STATUS_INVALID,
	REPLICATION_STATUS_VALID,
};

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

enum THREAD_EXIT_CODES {
	EXIT_ALIVE = 0,
	EXIT_THREAD,
	EXIT_PROCESS,
	EXIT_FLUSHING,
	EXIT_NOT_ACTIVE,
};


/* Selective features */
#define MIGRATE_FPU 0
#define MIGRATION_PROFILE	0
#define STATISTICS 0

#endif /* __INCLUDE_POPCORN_TYPES_H__ */
