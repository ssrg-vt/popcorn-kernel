/*
 * internal.h
 *
 * TODO Code refactoring is needed
 * This is just using a doubly linked list protected by a spinlock
 */

#ifndef KERNEL_POPCORN_INTERNAL_H_
#define KERNEL_POPCORN_INTERNAL_H_

#undef USE_DEBUG_MAPPINGS
#undef CHECK_FOR_DUPLICATES

#include <linux/list.h>
#include <linux/spinlock.h>
#include <linux/process_server.h>

extern struct list_head _count_head;
extern spinlock_t _count_head_lock;

extern struct list_head _memory_head;
extern spinlock_t _memory_head_lock;

extern struct list_head _vma_ack_head;
extern spinlock_t _vma_ack_head_lock;

void add_memory_entry(memory_t* entry);
int add_memory_entry_with_check(memory_t* entry);
memory_t* find_memory_entry(int cpu, int id);
int dump_memory_entries(memory_t * list[], int num, int *written);
void remove_memory_entry(memory_t* entry);

///////////////////////////////////////////////////////////////////////////////
// Specialized functions (TODO need massive refactoring)
///////////////////////////////////////////////////////////////////////////////

/*
 * Functions to manipulate count list
 * head: _count_head
 * lock: _count_head_lock
 */
static inline void add_count_entry(count_answers_t *entry)
{
	unsigned long flags;

	BUG_ON(!entry);

	spin_lock_irqsave(&_count_head_lock, flags);
	list_add_tail(&entry->list, &_count_head);
	spin_unlock_irqrestore(&_count_head_lock, flags);
}

static inline count_answers_t* find_count_entry(int cpu, int id)
{
	count_answers_t* e = NULL;
	count_answers_t* found = NULL;
	unsigned long flags;

	spin_lock_irqsave(&_count_head_lock, flags);
	list_for_each_entry(e, &_count_head, list) {
		if (e->tgroup_home_cpu == cpu && e->tgroup_home_id == id) {
#ifdef CHECK_FOR_DUPLICATES
			if (found) {
				printk(KERN_ERR"%s: duplicates in list %s %s (cpu %d id %d)\n",
							__func__, 
							found->waiting ? found->waiting->comm : "?",
							e->waiting ? e->waiting->comm : "?",
							cpu, id);
			}
			found = e;
#else
			found = e;
			break;
#endif
		}
	}
	spin_unlock_irqrestore(&_count_head_lock, flags);

	return found;
}

static inline void remove_count_entry(count_answers_t* entry)
{
	unsigned long flags;

	BUG_ON(!entry);

	spin_lock_irqsave(&_count_head_lock, flags);
	list_del_init(&entry->list);
	spin_unlock_irqrestore(&_count_head_lock, flags);
}


/*
 * Functions to manipulate vma_ack list
 * head: _vma_ack_head
 * lock: _vma_ack_head_lock
 */
static inline void add_vma_ack_entry(vma_op_answers_t* entry)
{
	unsigned long flags;

	if (!entry)
		return;

	spin_lock_irqsave(&_vma_ack_head_lock, flags);
	list_add_tail(&entry->list, &_vma_ack_head);
	spin_unlock_irqrestore(&_vma_ack_head_lock, flags);
}

static inline vma_op_answers_t* find_vma_ack_entry(int cpu, int id)
{
	vma_op_answers_t* v = NULL;
	vma_op_answers_t* found = NULL;
	unsigned long flags;

	spin_lock_irqsave(&_vma_ack_head_lock, flags);
	list_for_each_entry(v, &_vma_ack_head, list) {
		if (v->tgroup_home_cpu == cpu && v->tgroup_home_id == id) {
#ifdef CHECK_FOR_DUPLICATES
			if (found) {
				printk(KERN_ERR"%s: duplicates in list %s %s (cpu %d id %d)\n",
						__func__,
						found->waiting ? found->waiting->comm : "?",
						v->waiting ? v->waiting->comm : "?",
						cpu, id);
			}
			found = v;
#else
			found = v;
			break;
#endif
		}
	}
	spin_unlock_irqrestore(&_vma_ack_head_lock, flags);

	return found;
}

static inline void remove_vma_ack_entry(vma_op_answers_t* entry)
{
	unsigned long flags;

	spin_lock_irqsave(&_vma_ack_head_lock, flags);
	list_del_init(&entry->list);
	spin_unlock_irqrestore(&_vma_ack_head_lock, flags);
}

#endif /* KERNEL_POPCORN_INTERNAL_H_ */
