/*********************
 * process server header
 * dkatz
 *********************/

#ifndef _PROCESS_SERVER_H
#define _PROCESS_SERVER_H

#include <linux/pcn_kmsg.h> // Messaging

#include <process_server_arch_macros.h>
#include <popcorn/process_server_macro.h>
/*
 * Structures
 */
typedef struct _fetching_struct {
	struct _fetching_struct* next;
	struct _fetching_struct* prev;

	int tgroup_home_cpu;
	int tgroup_home_id;
	unsigned long vaddr;
	int status;
	int owners[MAX_KERNEL_IDS];
	int owner;
	long last_invalid;

} fetching_t;

typedef struct ack_answers_for_2_kernels {
	struct ack_answers_for_2_kernels* next;
	struct ack_answers_for_2_kernels* prev;

	//data_header_t header;
	int tgroup_home_cpu;
	int tgroup_home_id;
	unsigned long address;
	int response_arrived;
	struct task_struct * waiting;

} ack_answers_for_2_kernels_t;

#define VMA_OPERATION_FIELDS int tgroup_home_cpu; \
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

struct _vma_operation {
	VMA_OPERATION_FIELDS
};

typedef struct {
	struct pcn_kmsg_hdr header;
	union {
		struct {
			VMA_OPERATION_FIELDS
		};

#define VMA_OPERATION_PAD ((sizeof(struct _vma_operation)>PCN_KMSG_PAYLOAD_SIZE)?PAD_LONG_MESSAGE(sizeof(struct _vma_operation)):(PCN_KMSG_PAYLOAD_SIZE))

		char pad[VMA_OPERATION_PAD];
	}__attribute__((packed));

}__attribute__((packed)) vma_operation_t;

typedef struct _data_header {
	struct _data_header* next;
	struct _data_header* prev;
} data_header_t;

typedef struct {
	data_header_t head;
	struct task_struct * thread;
} shadow_thread_t;

struct _memory_struct;

typedef struct {
	data_header_t head;
	struct task_struct * main;
	shadow_thread_t* threads;
	raw_spinlock_t spinlock;
	struct _memory_struct* memory;
} thread_pull_t;

typedef struct _memory_struct {
	struct list_head list;

	int tgroup_home_cpu;
	int tgroup_home_id;
	struct mm_struct* mm;
	int alive;
	struct task_struct * main;

	int operation;
	unsigned long addr;
	unsigned long new_addr;
	size_t len;
	unsigned long new_len;
	unsigned long prot;
	unsigned long pgoff;
	unsigned long flags;
	char path[512];

	struct task_struct* waiting_for_main;
	struct task_struct* waiting_for_op;
	int arrived_op;
	int my_lock;
	int kernel_set[MAX_KERNEL_IDS];
	int exp_answ;
	int answers;
	int setting_up;
	spinlock_t lock_for_answer;
	struct rw_semaphore kernel_set_sem;
	vma_operation_t* message_push_operation;
	thread_pull_t* thread_pull;
	atomic_t pending_migration;
	atomic_t pending_back_migration;
} memory_t;

typedef struct count_answers {
	struct list_head list;

	int tgroup_home_cpu;
	int tgroup_home_id;
	int responses;
	int expected_responses;
	int count;
	raw_spinlock_t lock;
	struct task_struct * waiting;
} count_answers_t;

#define ACK_FIELDS int tgroup_home_cpu;\
		int tgroup_home_id; \
		unsigned long address; \
		int ack;\
		int writing; \
		unsigned long long time_stamp;

struct _ack {
	ACK_FIELDS
};

typedef struct {
	struct pcn_kmsg_hdr header;
	union {
		struct {
			ACK_FIELDS
		};

#define ACK_PAD ((sizeof(struct _ack)>PCN_KMSG_PAYLOAD_SIZE)?PAD_LONG_MESSAGE(sizeof(struct _ack)):(PCN_KMSG_PAYLOAD_SIZE))
		char pad[ACK_PAD];
	}__attribute__((packed));

}__attribute__((packed)) ack_t;

//int my_set[NR_CPUS]; saif changed
#define NEW_KERNEL_ANSWER_FIELDS int tgroup_home_cpu;\
		int tgroup_home_id; \
		int my_set[MAX_KERNEL_IDS];\
		int vma_operation_index;

struct _new_kernel_answer {
	NEW_KERNEL_ANSWER_FIELDS
};

typedef struct {
	struct pcn_kmsg_hdr header;
	union {
		struct {
			NEW_KERNEL_ANSWER_FIELDS
		};

#define NEW_KERNEL_ANSWER_PAD ((sizeof(struct _new_kernel_answer)>PCN_KMSG_PAYLOAD_SIZE)?PAD_LONG_MESSAGE(sizeof(struct _new_kernel_answer)):(PCN_KMSG_PAYLOAD_SIZE))
		char pad[NEW_KERNEL_ANSWER_PAD];
	}__attribute__((packed));

}__attribute__((packed)) new_kernel_answer_t;


#define BACK_MIGRATION_FIELDS unsigned int personality;\
		unsigned long def_flags;\
		pid_t placeholder_pid;\
		pid_t placeholder_tgid;\
		int back;\
		int prev_pid;\
		int tgroup_home_cpu;\
		int tgroup_home_id;\
		int origin_pid;\
		sigset_t remote_blocked, remote_real_blocked;\
		sigset_t remote_saved_sigmask;\
		struct sigpending remote_pending;\
		unsigned long sas_ss_sp;\
		size_t sas_ss_size;\
		struct k_sigaction action[_NSIG];

struct _back_migration_request {
		BACK_MIGRATION_FIELDS
		field_arch arch;
};

typedef struct {
	struct pcn_kmsg_hdr header;
	union {
		struct {
			BACK_MIGRATION_FIELDS
			field_arch arch;
		}__attribute__((packed));
#define	BACK_MIGRATION_STRUCT_PAD ((sizeof(struct _back_migration_request)>PCN_KMSG_PAYLOAD_SIZE)?PAD_LONG_MESSAGE(sizeof(struct _back_migration_request)):(PCN_KMSG_PAYLOAD_SIZE))
		char pad[BACK_MIGRATION_STRUCT_PAD];
	}__attribute__((packed));

}__attribute__((packed)) back_migration_request_t;


#define CLONE_FIELDS unsigned long stack_start; \
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
		int back;\
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
		void *popcorn_vdso;

struct _clone_request {
	CLONE_FIELDS
	field_arch arch;
};

typedef struct {
	struct pcn_kmsg_hdr header;
	union {
		struct {
			CLONE_FIELDS
			field_arch arch;
		}__attribute__((packed));
#define	CLONE_STRUCT_PAD ((sizeof(struct _clone_request)>PCN_KMSG_PAYLOAD_SIZE)?PAD_LONG_MESSAGE(sizeof(struct _clone_request)):(PCN_KMSG_PAYLOAD_SIZE))
		char pad[CLONE_STRUCT_PAD];
	}__attribute__((packed));
}__attribute__((packed)) clone_request_t;



/**
 * This message is sent in response to a clone request.
 * Its purpose is to notify the requesting cpu thatmak
 * the specified pid is executing on behalf of the
 * requesting cpu.
 */
#define PROCESS_PAIRING_FIELD int your_pid; \
		int my_pid;

struct _create_process_pairing{
	PROCESS_PAIRING_FIELD
};

typedef struct {

	struct pcn_kmsg_hdr header;

	union{

		struct{
			PROCESS_PAIRING_FIELD
		};

#define PROCESS_PAIRING_PAD ((sizeof(struct _create_process_pairing)>PCN_KMSG_PAYLOAD_SIZE)?PAD_LONG_MESSAGE(sizeof(struct _create_process_pairing)):(PCN_KMSG_PAYLOAD_SIZE))
		char pad[PROCESS_PAIRING_PAD];

	}__attribute__((packed)) ;

}__attribute__((packed))create_process_pairing_t;

#define COUNT_REQUEST_FIELD int tgroup_home_cpu; \
		int tgroup_home_id;

struct _remote_thread_count_request{
	COUNT_REQUEST_FIELD
};

typedef struct {
	struct pcn_kmsg_hdr header;

	union{

		struct{
			COUNT_REQUEST_FIELD
		};

#define COUNT_REQUEST_PAD ((sizeof(struct _remote_thread_count_request)>PCN_KMSG_PAYLOAD_SIZE)?PAD_LONG_MESSAGE(sizeof(struct _remote_thread_count_request)):(PCN_KMSG_PAYLOAD_SIZE))
		char pad[COUNT_REQUEST_PAD];
	}__attribute__((packed));

}__attribute__((packed)) remote_thread_count_request_t;

#define COUNT_RESPONSE_FIELD int tgroup_home_cpu; \
		int tgroup_home_id; \
		int count;

struct _remote_thread_count_response{
	COUNT_RESPONSE_FIELD
};

typedef struct {
	struct pcn_kmsg_hdr header;

	union{

		struct{
			COUNT_RESPONSE_FIELD
		};

#define COUNT_RESPONSE_PAD ((sizeof(struct _remote_thread_count_response)>PCN_KMSG_PAYLOAD_SIZE)?PAD_LONG_MESSAGE(sizeof(struct _remote_thread_count_response)):(PCN_KMSG_PAYLOAD_SIZE))
		char pad[COUNT_RESPONSE_PAD];
	}__attribute__((packed));

}__attribute__((packed)) remote_thread_count_response_t;

/**
 * This message informs the remote cpu of delegated
 * process death.  This occurs whether the process
 * is a placeholder or a delegate locally.
 */
#define EXITING_PROCESS_FIELDS  pid_t my_pid; \
		pid_t prev_pid;\
		int is_last_tgroup_member; \
		int group_exit;\
		long code;\

struct _exiting_process {
	EXITING_PROCESS_FIELDS
	field_arch arch;
};

typedef struct {
	struct pcn_kmsg_hdr header;
	union {
		struct {
			EXITING_PROCESS_FIELDS
			field_arch arch;
		};
#define EXITING_PROCES_PAD ((sizeof(struct _exiting_process)>PCN_KMSG_PAYLOAD_SIZE)?PAD_LONG_MESSAGE(sizeof(struct _exiting_process)):(PCN_KMSG_PAYLOAD_SIZE))
		char pad[EXITING_PROCES_PAD];
	}__attribute__((packed));

} __attribute__((packed)) exiting_process_t;

#define EXIT_GROUP_FIELDS int tgroup_home_cpu; \
		int tgroup_home_id;

struct _thread_group_exited_notification {
	EXIT_GROUP_FIELDS
};

typedef struct {
	struct pcn_kmsg_hdr header;

	union{

		struct{
			EXIT_GROUP_FIELDS
		};

#define EXITING_GROUP_PAD ((sizeof(struct _thread_group_exited_notification)>PCN_KMSG_PAYLOAD_SIZE)?PAD_LONG_MESSAGE(sizeof(struct _thread_group_exited_notification)):(PCN_KMSG_PAYLOAD_SIZE))
		char pad[EXITING_GROUP_PAD];

	}__attribute__((packed));


}__attribute__((packed)) thread_group_exited_notification_t;

typedef struct{
	struct pcn_kmsg_hdr header;
	char pad[PCN_KMSG_PAYLOAD_SIZE];

}__attribute__((packed)) create_thread_pull_t;
/**
 * Inform remote cpu of a vma to process mapping.
 */
typedef struct _vma_transfer {
	struct pcn_kmsg_hdr header;
	int vma_id;
	int clone_request_id;
	unsigned long start;
	unsigned long end;
	pgprot_t prot;
	unsigned long flags;
	unsigned long pgoff;
	char path[256];
}__attribute__((packed)) vma_transfer_t;

#define NEW_KERNEL_FIELDS int tgroup_home_cpu;\
		int tgroup_home_id;

struct _new_kernel {
	NEW_KERNEL_FIELDS
};

typedef struct {
	struct pcn_kmsg_hdr header;
	union {
		struct {
			NEW_KERNEL_FIELDS
		};

#define NEW_KERNEL_PAD ((sizeof(struct _new_kernel)>PCN_KMSG_PAYLOAD_SIZE)?PAD_LONG_MESSAGE(sizeof(struct _new_kernel)):(PCN_KMSG_PAYLOAD_SIZE))
		char pad[NEW_KERNEL_PAD];
	}__attribute__((packed));

}__attribute__((packed)) new_kernel_t;

#define MAPPING_FIELDS_FOR_2_KERNELS int tgroup_home_cpu; \
		int tgroup_home_id; \
		unsigned long address;\
		int is_write; \
		int is_fetch;\
		int vma_operation_index;\
		long last_write;

struct _mapping_for_2_kernels {
	MAPPING_FIELDS_FOR_2_KERNELS
};

typedef struct {
	struct pcn_kmsg_hdr header;
	union {
		struct {
			MAPPING_FIELDS_FOR_2_KERNELS
		};

#define MAPPING_FIELDS_FOR_2_KERNELS_PAD ((sizeof(struct _mapping_for_2_kernels)>PCN_KMSG_PAYLOAD_SIZE)?PAD_LONG_MESSAGE(sizeof(struct _mapping_for_2_kernels)):(PCN_KMSG_PAYLOAD_SIZE))
		char pad[MAPPING_FIELDS_FOR_2_KERNELS_PAD];
	}__attribute__((packed));

}__attribute__((packed)) data_request_for_2_kernels_t;

#define INVALID_FIELDS_FOR_2_KERNELS int tgroup_home_cpu;\
		int tgroup_home_id; \
		unsigned long address; \
		long last_write;\
		int vma_operation_index;

struct _invalid_for_2_kernels {
	INVALID_FIELDS_FOR_2_KERNELS
};

typedef struct {
	struct pcn_kmsg_hdr header;
	union {
		struct {
			INVALID_FIELDS_FOR_2_KERNELS
		};

#define INVALID_PAD ((sizeof(struct _invalid_for_2_kernels)>PCN_KMSG_PAYLOAD_SIZE)?PAD_LONG_MESSAGE(sizeof(struct _invalid_for_2_kernels)):(PCN_KMSG_PAYLOAD_SIZE))
		char pad[INVALID_PAD];
	}__attribute__((packed));

} __attribute__((packed)) invalid_data_for_2_kernels_t;

#define DATA_RESPONSE_FIELDS_FOR_2_KERNELS int tgroup_home_cpu; \
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
		char data; \

struct _data_response_for_2_kernels {
	DATA_RESPONSE_FIELDS_FOR_2_KERNELS
};

typedef struct {
	struct pcn_kmsg_hdr header;
	union {
		struct {
			DATA_RESPONSE_FIELDS_FOR_2_KERNELS
		};
#define DATA_RESPONSE_FIELDS_FOR_2_KERNELS_PAD ((sizeof(struct _data_response_for_2_kernels)>PCN_KMSG_PAYLOAD_SIZE)?PAD_LONG_MESSAGE(sizeof(struct _data_response_for_2_kernels)):(PCN_KMSG_PAYLOAD_SIZE))
		char pad[DATA_RESPONSE_FIELDS_FOR_2_KERNELS_PAD];
	}__attribute__((packed));

}__attribute__((packed)) data_response_for_2_kernels_t;

#define DATA_VOID_RESPONSE_FIELDS_FOR_2_KERNELS int tgroup_home_cpu; \
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
		int futex_owner; \

struct _data_void_response_for_2_kernels {
	DATA_VOID_RESPONSE_FIELDS_FOR_2_KERNELS
};

typedef struct {
	struct pcn_kmsg_hdr header;
	union {
		struct {
			DATA_VOID_RESPONSE_FIELDS_FOR_2_KERNELS
		};
#define DATA_VOID_RESPONSE_FIELDS_FOR_2_KERNELS_PAD ((sizeof(struct _data_void_response_for_2_kernels)>PCN_KMSG_PAYLOAD_SIZE)?PAD_LONG_MESSAGE(sizeof(struct _data_void_response_for_2_kernels)):(PCN_KMSG_PAYLOAD_SIZE))
		char pad[DATA_VOID_RESPONSE_FIELDS_FOR_2_KERNELS_PAD];
	}__attribute__((packed));

}__attribute__((packed)) data_void_response_for_2_kernels_t;

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

#define VMA_ACK_FIELDS int tgroup_home_cpu; \
		int tgroup_home_id; \
		int vma_operation_index;\
		unsigned long addr;

struct _vma_ack {
	VMA_ACK_FIELDS
};

typedef struct {
	struct pcn_kmsg_hdr header;
	union {
		struct {
			VMA_ACK_FIELDS
		};

#define VMA_ACK_PAD ((sizeof(struct _vma_ack)>PCN_KMSG_PAYLOAD_SIZE)?PAD_LONG_MESSAGE(sizeof(struct _vma_ack)):(PCN_KMSG_PAYLOAD_SIZE))
		char pad[VMA_ACK_PAD];

	}__attribute__((packed));

} __attribute__((packed)) vma_ack_t;

/*typedef struct _vma_ack{
 struct pcn_kmsg_hdr header;
 int tgroup_home_cpu; //4
 int tgroup_home_id; //4
 int vma_operation_index;
 char pad[PCN_KMSG_PAYLOAD_SIZE -12];
 }__attribute__((packed)) __attribute__((aligned(64))) vma_ack_t;
 */

#define UNMAP_FIELDS  int tgroup_home_cpu; \
		int tgroup_home_id;\
		unsigned long start;\
		size_t len;

struct _unmap_message {
	UNMAP_FIELDS
};

typedef struct {
	struct pcn_kmsg_hdr header;
	union {
		struct {
			UNMAP_FIELDS
		};
#define UNMAP_FIELDS_PAD ((sizeof(struct _unmap_message)>PCN_KMSG_PAYLOAD_SIZE)?PAD_LONG_MESSAGE(sizeof(struct _unmap_message)):(PCN_KMSG_PAYLOAD_SIZE))
		char pad[UNMAP_FIELDS_PAD];
	}__attribute__((packed));

} __attribute__((packed)) unmap_message_t;

#define VMA_LOCK_FIELDS int tgroup_home_cpu; \
		int tgroup_home_id; \
		int from_cpu;\
		int vma_operation_index;

struct _vma_lock {
	VMA_LOCK_FIELDS
};

typedef struct {
	struct pcn_kmsg_hdr header;
	union {
		struct {
			VMA_LOCK_FIELDS
		};
#define VMA_LOCK_PAD ((sizeof(struct _vma_lock)>PCN_KMSG_PAYLOAD_SIZE)?PAD_LONG_MESSAGE(sizeof(struct _vma_lock)):(PCN_KMSG_PAYLOAD_SIZE))

		char pad[VMA_LOCK_PAD];
	};

} vma_lock_t;

typedef struct vma_op_answers {
	struct list_head list;

	//data_header_t header;
	int tgroup_home_cpu;
	int tgroup_home_id;
	int responses;
	int expected_responses;
	int vma_operation_index;
	unsigned long address;
	struct task_struct *waiting;
	raw_spinlock_t lock;

} vma_op_answers_t;

typedef struct {
	struct delayed_work work;
	clone_request_t* request;
} clone_work_t;

typedef struct{
	struct work_struct work;
	back_migration_request_t* back_mig_request;
}back_mig_work_t;

typedef struct {
	struct delayed_work work;
	data_request_for_2_kernels_t* request;
	unsigned long address;
	int tgroup_home_cpu;
	int tgroup_home_id;
} request_work_t;

typedef struct {
	struct work_struct work;
	thread_group_exited_notification_t* request;
} exit_group_work_t;

typedef struct {
	struct work_struct work;
	ack_t* response;
} ack_work_t;

/*typedef struct {
	struct work_struct work;
	data_response_t* response;
} response_work_t;*/

typedef struct {
	struct delayed_work work;
	invalid_data_for_2_kernels_t* request;
} invalid_work_t;

typedef struct {
	struct work_struct work;
	new_kernel_t* request;
} new_kernel_work_t;

typedef struct {
	struct work_struct work;
	new_kernel_answer_t* answer;
	memory_t* memory;
} new_kernel_work_answer_t;

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

typedef struct info_page_walk {
	struct vm_area_struct* vma;
} info_page_walk_t;

typedef struct _sched_periodic_req {
	struct pcn_kmsg_hdr header;
	int power_1;
	int power_2;
	int power_3;
} sched_periodic_req;

/*
 * Migration hook.
 */
void synchronize_migrations(int tgroup_home_cpu,int tgroup_home_id );
int page_server_update_page(struct task_struct * tsk,struct mm_struct *mm, struct vm_area_struct *vma, unsigned long address, unsigned long page_fault_flags, int retrying);
int process_server_task_exit_notification(struct task_struct *tsk,long code);
int page_server_try_handle_mm_fault(struct task_struct *tsk,struct mm_struct *mm, struct vm_area_struct *vma,
		unsigned long address, unsigned long page_fault_flags,unsigned long error_code);
int process_server_dup_task(struct task_struct* orig, struct task_struct* task);

#endif // _PROCESS_SERVER_H
