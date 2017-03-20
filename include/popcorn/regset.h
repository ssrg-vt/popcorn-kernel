/*
 * File:
 * process_server_macros.h
 *
 * Description:
 * 	this file provides the architecture specific macro and structures of the
 *  helper functionality implementation of the process server
 *
 * Created on:
 * 	Sep 19, 2014
 *
 * Author:
 * 	Sharath Kumar Bhat, SSRG, VirginiaTech
 *
 */

#ifndef PROCESS_SERVER_ARCH_MACROS_H_
#define PROCESS_SERVER_ARCH_MACROS_H_

struct popcorn_regset_x86_64 {
	/* Program counter/instruction pointer */
	void* rip;

	/* General purpose registers */
	uint64_t rax, rdx, rcx, rbx,
			 rsi, rdi, rbp, rsp,
			 r8, r9, r10, r11,
			 r12, r13, r14, r15;

	/* Multimedia-extension (MMX) registers */
	uint64_t mmx[8];

	/* Streaming SIMD Extension (SSE) registers */
	unsigned __int128 xmm[16];

	/* x87 floating point registers */
	long double st[8];

	/* Segment registers */
	uint32_t cs, ss, ds, es, fs, gs;

	/* Flag register */
	uint64_t rflags;
};

struct popcorn_regset_aarch64 {
	/* Stack pointer & program counter */
	void* sp;
	void* pc;

	/* General purpose registers */
	uint64_t x[31];

	/* FPU/SIMD registers */
	unsigned __int128 v[32];
};

struct popcorn_regset_powerpc {

};

struct popcorn_regset_sparc {

};

#define FIELDS_ARCH \
	struct pt_regs regs;\
	unsigned long migration_ip;\
	unsigned long ip; \
	unsigned long bp;\
	unsigned long sp;\
	unsigned short thread_es;\
	unsigned short thread_ds;\
	unsigned long thread_fs;\
	unsigned short thread_fsindex;\
	unsigned long thread_gs;\
	unsigned short thread_gsindex; \
	unsigned int  task_flags;\
	unsigned char task_fpu_counter;\
	unsigned char thread_has_fpu;\
	struct popcorn_regset_x86_64 regs_x86;\
	struct popcorn_regset_aarch64 regs_aarch; \
	struct popcorn_regset_powerpc regs_powerpc; \
	struct popcorn_regset_sparc regs_sparc;
//	union thread_xstate fpu_state;

typedef struct _fields_arch {
	FIELDS_ARCH
} field_arch;

#endif
