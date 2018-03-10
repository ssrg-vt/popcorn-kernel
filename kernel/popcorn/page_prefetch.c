/*
 * Popcorn page prefetching machanism implementation
 */
#include <linux/mm.h>
#include <linux/slab.h>

#include "types.h"
#include "pgtable.h"
#include "wait_station.h"
#include "page_server.h"
#include "fh_action.h"

#include "page_prefetch.h"
//#include <popcorn/page_server.h>

struct fault_handle {
    struct hlist_node list;

    unsigned long addr;
    unsigned long flags;

    unsigned int limit;
    pid_t pid;
    int ret;

    atomic_t pendings;
    atomic_t pendings_retry;
    wait_queue_head_t waits;
    wait_queue_head_t waits_retry;
    struct remote_context *rc;

    struct completion *complete;
};

static inline int __fault_hash_key(unsigned long address)
{
    return (address >> PAGE_SHIFT) % FAULTS_HASH;
}

#define PER_PAGE_INFO_SIZE \
        (sizeof(unsigned long) * BITS_TO_LONGS(MAX_POPCORN_NODES))
#define PAGE_INFO_PER_REGION (PAGE_SIZE / PER_PAGE_INFO_SIZE)
static inline void __get_page_info_key(unsigned long addr, unsigned long *key, unsigned long *offset)
{
    unsigned long paddr = addr >> PAGE_SHIFT;
    *key = paddr / PAGE_INFO_PER_REGION;
    *offset = (paddr % PAGE_INFO_PER_REGION) *
            (PER_PAGE_INFO_SIZE / sizeof(unsigned long));
}

static inline unsigned long *__get_page_info(struct mm_struct *mm, unsigned long addr)
{
    unsigned long key, offset;
    unsigned long *region;
    struct remote_context *rc = mm->remote;
    __get_page_info_key(addr, &key, &offset);

    region = radix_tree_lookup(&rc->pages, key);
    if (!region) return NULL;

    return region + offset;
}

#define PI_FLAG_DISTRIBUTED 63
static inline bool page_is_mine(struct mm_struct *mm, unsigned long addr)
{
    unsigned long *pi = __get_page_info(mm, addr);

    if (!pi || !test_bit(PI_FLAG_DISTRIBUTED, pi)) return true;
    return test_bit(my_nid, pi);
}

static pte_t *__get_pte_at(struct mm_struct *mm, unsigned long addr, pmd_t **ppmd, spinlock_t **ptlp)
{
    pgd_t *pgd;
    pud_t *pud;
    pmd_t *pmd;

    pgd = pgd_offset(mm, addr);
    if (!pgd || pgd_none(*pgd)) return NULL;

    pud = pud_offset(pgd, addr);
    if (!pud || pud_none(*pud)) return NULL;

    pmd = pmd_offset(pud, addr);
    if (!pmd || pmd_none(*pmd)) return NULL;

    *ppmd = pmd;
    *ptlp = pte_lockptr(mm, pmd);

    return pte_offset_map(pmd, addr);
}

inline struct prefetch_list *alloc_prefetch_list(void)
{
	return kmalloc(sizeof(struct prefetch_list), GFP_KERNEL);
}

inline void free_prefetch_list(struct prefetch_list* pf_list)
{
	if (pf_list) kfree(pf_list);
}

inline void add_pf_list_at(struct prefetch_list* pf_list,
							unsigned long addr, int slot_num)
{
	//(struct prefetch_body*)((struct prefetch_body*)(pf_list) + slot_num)->addr = addr;
}


/*
 * Decide prefetched pages
 */
#define SKIP_NUM_OF_PAGES 20
#define PREFETCH_NUM_OF_PAGES 20
void prefetch_policy(struct prefetch_list* pf_list, unsigned long fault_addr)
{
	//TODO: prefetch some page fr testing
	static uint8_t cnt = 0;
	struct prefetch_body *list_ptr;
	cnt++;
	if (cnt > 100) {
		int i = 0;
		// prefetch
		list_ptr = (struct prefetch_body*)pf_list;
		for(i = 0; i < PREFETCH_NUM_OF_PAGES; i++) {
			list_ptr->addr = fault_addr + ((i + SKIP_NUM_OF_PAGES) * PAGE_SIZE);
			list_ptr++;
		}
    }
}

/*
 * Select prefetched pages
 * 		peek existing preftech_list
 * 		return a new preftechlist
 */
struct prefetch_list *select_prefetch_pages(
        struct prefetch_list* pf_list, struct mm_struct *mm)
{
    int slot = 0;
    struct prefetch_list *new_pf_list = NULL;
    struct prefetch_body *list_ptr = (struct prefetch_body*)pf_list;

    if (!(list_ptr->addr)) goto out;

    new_pf_list = alloc_prefetch_list();
    while (list_ptr->addr) {
		int fk;
		pmd_t *pmd;
		spinlock_t *ptl;
		bool found = false;
		unsigned long flags;
		struct fault_handle *fh;
        struct remote_context *rc;
        unsigned long addr = list_ptr->addr;
        __get_pte_at(mm, addr, &pmd, &ptl);
        spin_lock(ptl);

		rc = get_task_remote(current);
        fk = __fault_hash_key(addr);

		/* fault lock will stop next pte acess as well */
        spin_lock_irqsave(&rc->faults_lock[fk], flags); // if(!found) 
        spin_unlock(ptl);

		hlist_for_each_entry(fh, &rc->faults[fk], list) {
			if (fh->addr == addr) {
				found = true;
				break;
			}
		}

        if (!found && !page_is_mine(mm, addr)) { //leader
            add_pf_list_at(new_pf_list, addr, slot);
			slot++;
			/*
			fh = __alloc_fault_handle(tsk, addr);
			fh->flags |= fault_for_write(fault_flags) ? FAULT_HANDLE_WRITE : 0;
			fh->flags |= (fault_flags & FAULT_FLAG_REMOTE) ? FAULT_HANDLE_REMOTE : 0;
			*/
        } else { // follower	
		}
        list_ptr++;
		spin_unlock_irqrestore(&rc->faults_lock[fk], flags);
    }
out:
    free_prefetch_list(pf_list);
    return new_pf_list;
}


static void process_remote_prefetch_response(struct work_struct *work)
{
	//prefetch request
	START_KMSG_WORK(remote_prefetch_response_t, res, work);
	res->addr;
	res->populated;
	res->fh;


}

DEFINE_KMSG_WQ_HANDLER(remote_prefetch_response);
int __init page_prefetch_init(void)
{
    REGISTER_KMSG_WQ_HANDLER(
		PCN_KMSG_TYPE_REMOTE_PREFETCH_RESPONSE, remote_prefetch_response);

    return 0;
}
