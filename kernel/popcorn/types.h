#ifndef __POPCORN_TYPES_H__
#define __POPCORN_TYPES_H__

#include <linux/list.h>
#include <linux/spinlock.h>
#include <linux/completion.h>
#include <linux/workqueue.h>
#include <linux/signal.h>
#include <linux/rwsem.h>

#include <popcorn/pcn_kmsg.h> // Messaging
#include <popcorn/types.h>
#include <popcorn/bundle.h>

#include <asm/process_server.h>

#define VMA_OPERATION_FIELDS \
	int tgroup_home_cpu; \
	int tgroup_home_id; \
	int operation;\
	unsigned long addr;\
	unsigned long new_addr;\
	size_t len;\
	unsigned long new_len;\
	unsigned long prot;\
	unsigned long flags; \
	int from_cpu;\
	int vma_operation_index;\
	int pgoff;\
	char path[512];
DEFINE_PCN_KMSG(vma_operation_t, VMA_OPERATION_FIELDS);

#define VMA_LOCK_FIELDS \
	int tgroup_home_cpu; \
	int tgroup_home_id; \
	int from_cpu;\
	int vma_operation_index;
DEFINE_PCN_KMSG(vma_lock_t, VMA_LOCK_FIELDS);

#define VMA_ACK_FIELDS \
	int tgroup_home_cpu; \
	int tgroup_home_id; \
	int vma_operation_index;\
	unsigned long addr;
DEFINE_PCN_KMSG(vma_ack_t, VMA_ACK_FIELDS);

#define UNMAP_FIELDS \
	int tgroup_home_cpu; \
	int tgroup_home_id;\
	unsigned long start;\
	size_t len;
DEFINE_PCN_KMSG(unmap_message_t, UNMAP_FIELDS);


typedef struct _memory_struct {
	struct list_head list;
	atomic_t count;

	int tgroup_home_cpu;
	int tgroup_home_id;
	struct mm_struct* mm;
	int alive;
	int setting_up;

	struct task_struct *helper;
	bool helper_stop;

	struct task_struct *shadow_spawner;
	struct completion spawn_egg;
	struct list_head shadow_eggs;
	spinlock_t shadow_eggs_lock;
	atomic_t pending_migration;
	atomic_t pending_back_migration;

	char path[512];

	int operation;
	unsigned long addr;
	unsigned long new_addr;
	size_t len;
	unsigned long new_len;
	unsigned long prot;
	unsigned long pgoff;
	unsigned long flags;

	struct task_struct* waiting_for_main;
	struct task_struct* waiting_for_op;
	int arrived_op;
	int my_lock;

	int kernel_set[MAX_POPCORN_NODES];	/* TODO: change to bitmap */
	struct rw_semaphore kernel_set_sem;

	struct list_head pages;
	spinlock_t pages_lock;

	vma_operation_t *message_push_operation;
	atomic_t answers_remain;
} memory_t;


void add_memory_entry_in_out(memory_t *m, int nid, bool in);
#define add_memory_entry_in(m, nid)	\
	add_memory_entry_in_out(m, nid, true)
#define add_memory_entry_out(m, nid)	\
	add_memory_entry_in_out(m, nid, false)

memory_t *find_memory_entry_in_out(int nid, int pid, bool in);
#define find_memory_entry_in(nid, pid) \
	find_memory_entry_in_out(nid, pid, true)
#define find_memory_entry_out(nid, pid) \
	find_memory_entry_in_out(nid, pid, false)

void remove_memory_entry_in_out(memory_t *m, bool in);
#define remove_memory_entry_in(m) \
	remove_memory_entry_in_out(m, true)
#define remove_memory_entry_out(m) \
	remove_memory_entry_in_out(m, false)

void add_memory_entry(memory_t* entry);
int add_memory_entry_with_check(memory_t* entry);
memory_t* find_memory_entry(int cpu, int id);
int dump_memory_entries(memory_t * list[], int num, int *written);
void remove_memory_entry(memory_t* entry);


#define ACK_FIELDS \
	int tgroup_home_cpu;\
	int tgroup_home_id; \
	unsigned long address; \
	int ack;\
	int writing; \
	unsigned long long time_stamp;
DEFINE_PCN_KMSG(ack_t, ACK_FIELDS);

#define NEW_KERNEL_FIELDS \
	int tgroup_home_cpu;\
	int tgroup_home_id;
DEFINE_PCN_KMSG(new_kernel_t, NEW_KERNEL_FIELDS);

#define NEW_KERNEL_RESPONSE_FIELDS \
	int tgroup_home_cpu;\
	int tgroup_home_id; \
	int my_set[MAX_KERNEL_IDS];\
	int vma_operation_index;
DEFINE_PCN_KMSG(new_kernel_response_t, NEW_KERNEL_RESPONSE_FIELDS);

#define BACK_MIGRATION_FIELDS \
	unsigned int personality;\
	unsigned long def_flags;\
	pid_t placeholder_pid;\
	pid_t placeholder_tgid;\
	int prev_pid;\
	int tgroup_home_cpu;\
	int tgroup_home_id;\
	int origin_pid;\
	sigset_t remote_blocked, remote_real_blocked;\
	sigset_t remote_saved_sigmask;\
	struct sigpending remote_pending;\
	unsigned long sas_ss_sp;\
	size_t sas_ss_size;\
	struct k_sigaction action[_NSIG]; \
	field_arch arch;
DEFINE_PCN_KMSG(back_migration_request_t, BACK_MIGRATION_FIELDS);

#define CLONE_FIELDS \
	unsigned long stack_start; \
	unsigned long env_start;\
	unsigned long env_end;\
	unsigned long arg_start;\
	unsigned long arg_end;\
	unsigned long start_brk;\
	unsigned long brk;\
	unsigned long start_code ;\
	unsigned long end_code;\
	unsigned long start_data;\
	unsigned long end_data ;\
	unsigned int personality;\
	unsigned long def_flags;\
	char exe_path[512];\
	pid_t placeholder_pid;\
	pid_t placeholder_tgid;\
	int prev_pid;\
	int tgroup_home_cpu;\
	int tgroup_home_id;\
	int origin_pid;\
	sigset_t remote_blocked, remote_real_blocked;\
	sigset_t remote_saved_sigmask;\
	struct sigpending remote_pending;\
	unsigned long sas_ss_sp;\
	size_t sas_ss_size;\
	struct k_sigaction action[_NSIG];\
	void *popcorn_vdso; \
	field_arch arch;
DEFINE_PCN_KMSG(clone_request_t, CLONE_FIELDS);


/**
 * This message is sent in response to a clone request.
 * Its purpose is to notify the requesting cpu that make
 * the specified pid is executing on behalf of the
 * requesting cpu.
 */
#define PROCESS_PAIRING_FIELDS \
	int your_pid; \
	int my_pid;
DEFINE_PCN_KMSG(create_process_pairing_t, PROCESS_PAIRING_FIELDS);

#define COUNT_REQUEST_FIELDS \
	int tgroup_home_cpu; \
	int tgroup_home_id;

DEFINE_PCN_KMSG(remote_thread_count_request_t, COUNT_REQUEST_FIELDS);

#define COUNT_RESPONSE_FIELDS \
	int tgroup_home_cpu; \
	int tgroup_home_id; \
	int count;
DEFINE_PCN_KMSG(remote_thread_count_response_t, COUNT_RESPONSE_FIELDS);

/**
 * This message informs the remote cpu of delegated
 * process death. This occurs whether the process
 * is a placeholder or a delegate locally.
 */
#define EXITING_PROCESS_FIELDS  \
	pid_t my_pid; \
	pid_t prev_pid;\
	int is_last_tgroup_member; \
	int group_exit;\
	long code;\
	field_arch arch;
DEFINE_PCN_KMSG(exiting_process_t, EXITING_PROCESS_FIELDS);

#define EXIT_GROUP_FIELDS	\
	int tgroup_home_cpu; \
	int tgroup_home_id;
DEFINE_PCN_KMSG(thread_group_exited_notification_t, EXIT_GROUP_FIELDS);


/**
 * Inform remote cpu of a vma to process mapping.
 */
#define VMA_TRANSFER_FIELDS \
	int vma_id; \
	int clone_request_id; \
	unsigned long start; \
	unsigned long end; \
	pgprot_t prot; \
	unsigned long flags; \
	unsigned long pgoff; \
	char path[256];
DEFINE_PCN_KMSG(vma_transfer_t, VMA_TRANSFER_FIELDS);

#define MAPPING_FIELDS_FOR_2_KERNELS \
	int tgroup_home_cpu; \
	int tgroup_home_id; \
	unsigned long address;\
	int is_write; \
	int is_fetch;\
	int vma_operation_index;\
	long last_write;
DEFINE_PCN_KMSG(data_request_for_2_kernels_t, MAPPING_FIELDS_FOR_2_KERNELS);

#define INVALID_FIELDS_FOR_2_KERNELS \
	int tgroup_home_cpu;\
	int tgroup_home_id; \
	unsigned long address; \
	long last_write;\
	int vma_operation_index;
DEFINE_PCN_KMSG(invalid_data_for_2_kernels_t, INVALID_FIELDS_FOR_2_KERNELS);

#define DATA_RESPONSE_FIELDS \
	int tgroup_home_cpu; \
	int tgroup_home_id;  \
	unsigned long address; \
	__wsum checksum; \
	long last_write;\
	int owner;\
	int vma_present; \
	unsigned long vaddr_start;\
	unsigned long vaddr_size;\
	pgprot_t prot; \
	unsigned long vm_flags; \
	unsigned long pgoff;\
	char path[512];\
	unsigned int data_size;\
	int diff;\
	int futex_owner; \
	char data;
DEFINE_PCN_KMSG(data_response_for_2_kernels_t,DATA_RESPONSE_FIELDS);

#define DATA_VOID_RESPONSE_FIELDS \
	int tgroup_home_cpu; \
	int tgroup_home_id;  \
	unsigned long address; \
	int vma_present; \
	unsigned long vaddr_start;\
	unsigned long vaddr_size;\
	unsigned long vm_flags; \
	unsigned long pgoff;\
	char path[512];\
	pgprot_t prot; \
	int fetching_read; \
	int fetching_write;\
	int owner;\
	__wsum checksum; \
	int futex_owner;
DEFINE_PCN_KMSG(data_void_response_for_2_kernels_t, DATA_VOID_RESPONSE_FIELDS);


typedef struct mapping_answers_2_kernels {
	struct mapping_answers_2_kernels* next;
	struct mapping_answers_2_kernels* prev;

	int tgroup_home_cpu;
	int tgroup_home_id;
	unsigned long address;
	int vma_present;
	unsigned long vaddr_start;
	unsigned long vaddr_size;
	unsigned long pgoff;
	char path[512];
	pgprot_t prot;
	unsigned long vm_flags;
	int is_write;
	int is_fetch;
	int owner;
	int address_present;
	long last_write;
	int owners [MAX_KERNEL_IDS];
	data_response_for_2_kernels_t* data;
	int arrived_response;
	struct task_struct* waiting;
	int futex_owner;
} mapping_answers_for_2_kernels_t;


#define REMOTE_PAGE_REQUEST_FIELDS \
	int tgroup_home_cpu; \
	int tgroup_home_id; \
	int remote_pid; \
	bool is_write; \
	unsigned long addr;
DEFINE_PCN_KMSG(remote_page_request_t, REMOTE_PAGE_REQUEST_FIELDS);

enum remote_page_status {
	RP_STATUS_FETCHED = 0,
	RP_STATUS_KILLED,
	RP_STATUS_NOEXIST,
	RP_STATUS_FAULT,
};

#define REMOTE_PAGE_RESPONSE_FIELDS \
	int tgroup_home_cpu; \
	int tgroup_home_id; \
	int remote_pid;	\
	int result; \
	unsigned long addr; \
	unsigned long vm_start; \
	unsigned long vm_end; \
	unsigned long vm_flags;	\
	unsigned long vm_pgoff; \
	char vm_file_path[512]; \
	char page[PAGE_SIZE];
DEFINE_PCN_KMSG(remote_page_response_t, REMOTE_PAGE_RESPONSE_FIELDS);
#define remote_page_anon(x) ((x)->vm_file_path[0] == '\0' ? true : false)

#define REMOTE_PAGE_INVALIDATE_FIELDS \
	int tgroup_home_cpu; \
	int tgroup_home_id; \
	unsigned long addr;
DEFINE_PCN_KMSG(remote_page_invalidate_t, REMOTE_PAGE_INVALIDATE_FIELDS);

/**
 * Works
 */

/* Process server */

typedef struct {
	struct work_struct work;
	exiting_process_t* request;
} exit_work_t;

typedef struct {
	struct work_struct work;
	remote_thread_count_request_t* request;
} count_work_t;

typedef struct {
	struct work_struct work;
	new_kernel_t* request;
} new_kernel_work_t;

typedef struct {
	struct work_struct work;
	new_kernel_response_t* answer;
	memory_t* memory;
} new_kernel_work_answer_t;

typedef struct {
	struct delayed_work work;
	clone_request_t* request;
} clone_work_t;

typedef struct{
	struct work_struct work;
	back_migration_request_t* back_mig_request;
} back_mig_work_t;

typedef struct {
	struct work_struct work;
	thread_group_exited_notification_t* request;
} exit_group_work_t;


/* Page server */

typedef struct {
	struct delayed_work work;
	data_request_for_2_kernels_t* request;
	unsigned long address;
	int tgroup_home_cpu;
	int tgroup_home_id;
} request_work_t;

typedef struct {
	struct work_struct work;
	ack_t* response;
} ack_work_t;

typedef struct {
	struct delayed_work work;
	invalid_data_for_2_kernels_t* request;
} invalid_work_t;


/* vma server */

typedef struct {
	struct work_struct work;
	vma_operation_t* operation;
	memory_t* memory;
	int fake;
} vma_op_work_t;

typedef struct {
	struct work_struct work;
	unmap_message_t* unmap;
	int fake;
	memory_t* memory;
} vma_unmap_work_t;

typedef struct {
	struct work_struct work;
	vma_lock_t* lock;
	memory_t* memory;
} vma_lock_work_t;


/* sched server */

typedef struct _sched_periodic_req {
	struct pcn_kmsg_hdr header;
	int power_1;
	int power_2;
	int power_3;
} sched_periodic_req;

#endif /* __TYPES_H__ */
