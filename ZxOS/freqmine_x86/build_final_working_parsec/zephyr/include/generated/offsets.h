/* THIS FILE IS AUTO GENERATED.  PLEASE DO NOT EDIT.
 *
 * This header file provides macros for the offsets of various structure
 * members.  These offset macros are primarily intended to be used in
 * assembly code.
 */

#ifndef __GEN_OFFSETS_H__
#define __GEN_OFFSETS_H__

#define ___cpu_t_current_OFFSET 0x10
#define ___cpu_t_nested_OFFSET 0x0
#define ___cpu_t_irq_stack_OFFSET 0x8
#define ___cpu_t_arch_OFFSET 0x25
#define ___cpu_t_SIZEOF 0x28
#define ___kernel_t_cpus_OFFSET 0x0
#define ___kernel_t_ready_q_OFFSET 0x28
#define ___ready_q_t_cache_OFFSET 0x0
#define ___thread_base_t_user_options_OFFSET 0x18
#define ___thread_base_t_thread_state_OFFSET 0x19
#define ___thread_base_t_prio_OFFSET 0x1a
#define ___thread_base_t_sched_locked_OFFSET 0x1b
#define ___thread_base_t_preempt_OFFSET 0x1a
#define ___thread_base_t_swap_data_OFFSET 0x20
#define ___thread_t_base_OFFSET 0x0
#define ___thread_t_callee_saved_OFFSET 0x48
#define ___thread_t_arch_OFFSET 0xd0
#define ___thread_t_switch_handle_OFFSET 0xb8
#define K_THREAD_SIZEOF 0x320
#define _DEVICE_STRUCT_SIZEOF 0x30
#define _DEVICE_STRUCT_HANDLES_OFFSET 0x28
#define ___callee_saved_t_rsp_OFFSET 0x0
#define ___callee_saved_t_rbp_OFFSET 0x10
#define ___callee_saved_t_rbx_OFFSET 0x8
#define ___callee_saved_t_r12_OFFSET 0x18
#define ___callee_saved_t_r13_OFFSET 0x20
#define ___callee_saved_t_r14_OFFSET 0x28
#define ___callee_saved_t_r15_OFFSET 0x30
#define ___callee_saved_t_rip_OFFSET 0x38
#define ___callee_saved_t_rflags_OFFSET 0x40
#define ___thread_arch_t_rax_OFFSET 0x8
#define ___thread_arch_t_rcx_OFFSET 0x10
#define ___thread_arch_t_rdx_OFFSET 0x18
#define ___thread_arch_t_rsi_OFFSET 0x20
#define ___thread_arch_t_rdi_OFFSET 0x28
#define ___thread_arch_t_r8_OFFSET 0x30
#define ___thread_arch_t_r9_OFFSET 0x38
#define ___thread_arch_t_r10_OFFSET 0x40
#define ___thread_arch_t_r11_OFFSET 0x48
#define ___thread_arch_t_sse_OFFSET 0x50
#define __x86_tss64_t_ist1_OFFSET 0x24
#define __x86_tss64_t_ist2_OFFSET 0x2c
#define __x86_tss64_t_ist6_OFFSET 0x4c
#define __x86_tss64_t_ist7_OFFSET 0x54
#define __x86_tss64_t_cpu_OFFSET 0x68
#define __X86_TSS64_SIZEOF 0x70
#define __x86_cpuboot_t_ready_OFFSET 0x0
#define __x86_cpuboot_t_tr_OFFSET 0x4
#define __x86_cpuboot_t_gs_base_OFFSET 0x8
#define __x86_cpuboot_t_sp_OFFSET 0x10
#define __x86_cpuboot_t_stack_size_OFFSET 0x18
#define __x86_cpuboot_t_fn_OFFSET 0x20
#define __x86_cpuboot_t_arg_OFFSET 0x28
#define __X86_CPUBOOT_SIZEOF 0x30
#define ___thread_arch_t_flags_OFFSET 0x0

#endif /* __GEN_OFFSETS_H__ */
