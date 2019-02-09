/**
 * @file vma_server.c
 *
 * Popcorn Linux VMA handler implementation
 * This work was an extension of David Katz MS Thesis, but totally rewritten 
 * by Sang-Hoon to support multithread environment.
 *
 * @author Sang-Hoon Kim, SSRG Virginia Tech 2016-2017
 * @author Vincent Legout, Antonio Barbalace, SSRG Virginia Tech 2016
 * @author Ajith Saya, Sharath Bhat, SSRG Virginia Tech 2015
 * @author Marina Sadini, Antonio Barbalace, SSRG Virginia Tech 2014
 * @author Marina Sadini, SSRG Virginia Tech 2013
 */

#include <linux/slab.h>
#include <linux/mm.h>
#include <linux/kthread.h>

#include <linux/mman.h>
#include <linux/highmem.h>
#include <linux/ptrace.h>
#include <linux/syscalls.h>

#include <linux/elf.h>

#include <popcorn/types.h>
#include <popcorn/bundle.h>

#include "types.h"
#include "util.h"
#include "vma_server.h"
#include "page_server.h"
#include "wait_station.h"

enum vma_op_code {
	VMA_OP_NOP = -1,
	VMA_OP_MMAP,
	VMA_OP_MUNMAP,
	VMA_OP_MPROTECT,
	VMA_OP_MREMAP,
	VMA_OP_MADVISE,
	VMA_OP_BRK,
	VMA_OP_MAX,
};

const char *vma_op_code_sz[] = {
	"mmap", "munmap", "mprotect", "mremap", "madvise", "brk"
};


static unsigned long map_difference(struct mm_struct *mm, struct file *file,
		unsigned long start, unsigned long end,
		unsigned long prot, unsigned long flags, unsigned long pgoff)
{
	unsigned long ret = start;
	unsigned long error;
	unsigned long populate = 0;
	struct vm_area_struct* vma;

	/**
	 * Go through ALL VMAs, looking for overlapping with this space.
	 */
	VSPRINTK("  [%d] map+ %lx %lx\n", current->pid, start, end);
	for (vma = current->mm->mmap; start < end; vma = vma->vm_next) {
		/*
		VSPRINTK("  [%d] vma  %lx -- %lx\n", current->pid,
				vma ? vma->vm_start : 0, vma ? vma->vm_end : 0);
		*/
		if (vma == NULL || end <= vma->vm_start) {
			/**
			 * We've reached the end of the list, or the VMA is fully
			 * above the region of interest
			 */
			VSPRINTK("  [%d] map0 %lx -- %lx @ %lx, %lx\n", current->pid,
					start, end, pgoff, prot);
			error = do_mmap_pgoff(file, start, end - start,
					prot, flags, pgoff, &populate);
			if (error != start) {
				ret = VM_FAULT_SIGBUS;
			}
			break;
		} else if (start >= vma->vm_start && end <= vma->vm_end) {
			/**
			 * VMA fully encompases the region of interest. nothing to do
			 */
			break;
		} else if (start >= vma->vm_start
				&& start < vma->vm_end && end > vma->vm_end) {
			/**
			 * VMA includes the start of the region of interest
			 * but not the end. advance start (no mapping to do)
			 */
			pgoff += ((vma->vm_end - start) >> PAGE_SHIFT);
			start = vma->vm_end;
		} else if (start < vma->vm_start
				&& vma->vm_start < end && end <= vma->vm_end) {
			/**
			 * VMA includes the end of the region of interest
			 * but not the start
			 */
			VSPRINTK("  [%d] map1 %lx -- %lx @ %lx\n", current->pid,
					start, vma->vm_start, pgoff);
			error = do_mmap_pgoff(file, start, vma->vm_start - start,
					prot, flags, pgoff, &populate);
			if (error != start) {
				ret = VM_FAULT_SIGBUS;;
			}
			break;
		} else if (start <= vma->vm_start && vma->vm_end <= end) {
			/* VMA is fully within the region of interest */
			VSPRINTK("  [%d] map2 %lx -- %lx @ %lx\n", current->pid,
					start, vma->vm_start, pgoff);
			error = do_mmap_pgoff(file, start, vma->vm_start - start,
					prot, flags, pgoff, &populate);
			if (error != start) {
				ret = VM_FAULT_SIGBUS;
				break;
			}

			/**
			 * Then advance to the end of this VMA
			 */
			pgoff += ((vma->vm_end - start) >> PAGE_SHIFT);
			start = vma->vm_end;
		}
	}
	BUG_ON(populate);
	return ret;
}


#if 0
/**
 * Heterogeneous binary support
 *
 * Handle misaligned ELF sections in the heterogeneous binary.
 * However, recent alignment tool updates makes ELF sections aligned,
 * so this is not required anymore
 * Should be paried to fs/binfmt_elf.c
 */
static unsigned long __get_file_offset(struct file *file, unsigned long vm_start)
{
	struct elfhdr elf_ex;
	struct elf_phdr *elf_eppnt = NULL, *elf_eppnt_start = NULL;
	int size, retval, i;

	retval = kernel_read(file, 0, (char *)&elf_ex, sizeof(elf_ex));
	if (retval != sizeof(elf_ex)) {
		printk("%s: ERROR in Kernel read of ELF file\n", __func__);
		retval = -1;
		goto out;
	}

	size = elf_ex.e_phnum * sizeof(struct elf_phdr);

	elf_eppnt = kmalloc(size, GFP_KERNEL);
	if (elf_eppnt == NULL) {
		printk("%s: ERROR: kmalloc failed in\n", __func__);
		retval = -1;
		goto out;
	}

	elf_eppnt_start = elf_eppnt;
	retval = kernel_read(file, elf_ex.e_phoff, (char *)elf_eppnt, size);
	if (retval != size) {
		printk("%s: ERROR: during kernel read of ELF file\n", __func__);
		retval = -1;
		goto out;
	}
	retval = 0;
	for (i = 0; i < elf_ex.e_phnum; i++, elf_eppnt++) {
		if (elf_eppnt->p_type != PT_LOAD) continue;

		if ((vm_start >= elf_eppnt->p_vaddr) &&
				(vm_start <= (elf_eppnt->p_vaddr + elf_eppnt->p_memsz))) {
			retval = elf_eppnt->p_offset +
				(vm_start & PAGE_MASK) - (elf_eppnt->p_vaddr & PAGE_MASK);
			retval >>= PAGE_SHIFT;
			break;
		}
	}

out:
	if (elf_eppnt_start != NULL)
		kfree(elf_eppnt_start);

	return retval;
}
#endif


/**
 * VMA operation delegators at remotes
 */
static vma_op_request_t *__alloc_vma_op_request(enum vma_op_code opcode)
{
	vma_op_request_t *req = kmalloc(sizeof(*req), GFP_KERNEL);

	req->origin_pid = current->origin_pid,
	req->remote_pid = current->pid,
	req->operation = opcode;

	return req;
}

static int __delegate_vma_op(vma_op_request_t *req, vma_op_response_t **resp)
{
	vma_op_response_t *res;
	struct wait_station *ws = get_wait_station(current);

	req->remote_ws = ws->id;

	pcn_kmsg_send(PCN_KMSG_TYPE_VMA_OP_REQUEST,
			current->origin_nid, req, sizeof(*req));
	res = wait_at_station(ws);
	BUG_ON(res->operation != req->operation);

	*resp = res;
	return res->ret;
}

static int handle_vma_op_response(struct pcn_kmsg_message *msg)
{
	vma_op_response_t *res = (vma_op_response_t *)msg;
	struct wait_station *ws = wait_station(res->remote_ws);

	ws->private = res;
	complete(&ws->pendings);

	return 0;
}

unsigned long vma_server_mmap_remote(struct file *file,
		unsigned long addr, unsigned long len,
		unsigned long prot, unsigned long flags, unsigned long pgoff)
{
	unsigned long ret = 0;
	vma_op_request_t *req = __alloc_vma_op_request(VMA_OP_MMAP);
	vma_op_response_t *res;

	req->addr = addr;
	req->len = len;
	req->prot = prot;
	req->flags = flags;
	req->pgoff = pgoff;
	get_file_path(file, req->path, sizeof(req->path));

	VSPRINTK("\n## VMA mmap [%d] %lx - %lx, %lx %lx\n", current->pid,
			addr, addr + len, prot, flags);
	if (req->path[0] != '\0') {
		VSPRINTK("  [%d] %s\n", current->pid, req->path);
	}

	ret = __delegate_vma_op(req, &res);

	VSPRINTK("  [%d] %ld %lx -- %lx\n", current->pid,
			ret, res->addr, res->addr + res->len);

	if (ret) goto out_free;

	while (!down_write_trylock(&current->mm->mmap_sem)) {
		schedule();
	}
	ret = map_difference(current->mm, file, res->addr, res->addr + res->len,
			prot, flags, pgoff);
	up_write(&current->mm->mmap_sem);

out_free:
	kfree(req);
	pcn_kmsg_done(res);

	return ret;
}

int vma_server_munmap_remote(unsigned long start, size_t len)
{
	int ret;
	vma_op_request_t *req;
	vma_op_response_t *res;

	VSPRINTK("\n## VMA munmap [%d] %lx %lx\n", current->pid, start, len);

	ret = vm_munmap(start, len);
	if (ret) return ret;

	req = __alloc_vma_op_request(VMA_OP_MUNMAP);
	req->addr = start;
	req->len = len;

	ret = __delegate_vma_op(req, &res);

	VSPRINTK("  [%d] %d %lx -- %lx\n", current->pid,
			ret, res->addr, res->addr + res->len);

	kfree(req);
	pcn_kmsg_done(res);

	return ret;
}

int vma_server_brk_remote(unsigned long oldbrk, unsigned long brk)
{
	int ret;
	vma_op_request_t *req = __alloc_vma_op_request(VMA_OP_BRK);
	vma_op_response_t *res;

	req->brk = brk;

	VSPRINTK("\n## VMA brk-ed [%d] %lx --> %lx\n", current->pid, oldbrk, brk);

	ret = __delegate_vma_op(req, &res);

	VSPRINTK("  [%d] %d %lx\n", current->pid, ret, res->brk);

	kfree(req);
	pcn_kmsg_done(res);

	return ret;
}

int vma_server_madvise_remote(unsigned long start, size_t len, int behavior)
{
	int ret;
	vma_op_request_t *req = __alloc_vma_op_request(VMA_OP_MADVISE);
	vma_op_response_t *res;

	req->addr = start;
	req->len = len;
	req->behavior = behavior;

	VSPRINTK("\n## VMA madvise-d [%d] %lx %lx %d\n", current->pid,
			start, len, behavior);

	ret = __delegate_vma_op(req, &res);

	VSPRINTK("  [%d] %d %lx -- %lx %d\n", current->pid,
			ret, res->addr, res->addr + res->len, behavior);

	kfree(req);
	pcn_kmsg_done(res);

	return ret;
}

int vma_server_mprotect_remote(unsigned long start, size_t len, unsigned long prot)
{
	int ret;
	vma_op_request_t *req = __alloc_vma_op_request(VMA_OP_MPROTECT);
	vma_op_response_t *res;

	req->start = start;
	req->len = len;
	req->prot = prot;

	VSPRINTK("\nVMA mprotect [%d] %lx %lx %lx\n", current->pid,
			start, len, prot);

	ret = __delegate_vma_op(req, &res);

	VSPRINTK("  [%d] %d %lx -- %lx %lx\n", current->pid,
			ret, res->start, res->start + res->len, prot);

	kfree(req);
	pcn_kmsg_done(res);

	return ret;
}

int vma_server_mremap_remote(unsigned long addr, unsigned long old_len,
		unsigned long new_len, unsigned long flags, unsigned long new_addr)
{
	WARN_ON_ONCE("Does not support remote mremap yet");
	VSPRINTK("\nVMA mremap [%d] %lx %lx %lx %lx %lx\n", current->pid,
			addr, old_len, new_len, flags, new_addr);
	return -EINVAL;
}


/**
 * VMA handlers for origin
 */
int vma_server_munmap_origin(unsigned long start, size_t len, int nid_except)
{
	int nid;
	vma_op_request_t *req = __alloc_vma_op_request(VMA_OP_MUNMAP);
	struct remote_context *rc = get_task_remote(current);

	req->start = start;
	req->len = len;

	for (nid = 0; nid < MAX_POPCORN_NODES; nid++) {
		struct wait_station *ws;
		vma_op_response_t *res;

		if (!get_popcorn_node_online(nid) || !rc->remote_tgids[nid]) continue;

		if (nid == my_nid || nid == nid_except) continue;

		ws = get_wait_station(current);
		req->remote_ws = ws->id;
		req->origin_pid = rc->remote_tgids[nid];

		VSPRINTK("  [%d] ->munmap [%d/%d] %lx+%lx (%lx)\n", current->pid,
				req->origin_pid, nid, start, len,
				instruction_pointer(current_pt_regs()));
		pcn_kmsg_send(PCN_KMSG_TYPE_VMA_OP_REQUEST, nid, req, sizeof(*req));
		res = wait_at_station(ws);
		pcn_kmsg_done(res);
	}
	put_task_remote(current);
	kfree(req);

	vm_munmap(start, len);
	return 0;
}


/**
 * VMA worker
 *
 * We do this stupid thing because functions related to meomry mapping operate
 * on "current". Thus, we need mmap/munmap/madvise in our process
 */
static void __reply_vma_op(vma_op_request_t *req, long ret)
{
	vma_op_response_t *res = pcn_kmsg_get(sizeof(*res));

	res->origin_pid = current->pid;
	res->remote_pid = req->remote_pid;
	res->remote_ws = req->remote_ws;

	res->operation = req->operation;
	res->ret = ret;
	res->addr = req->addr;
	res->len = req->len;

	pcn_kmsg_post(PCN_KMSG_TYPE_VMA_OP_RESPONSE,
			PCN_KMSG_FROM_NID(req), res, sizeof(*res));
}


/**
 * Handle delegated VMA operations
 * Currently, the remote worker only handles munmap VMA operations.
 */
static long __process_vma_op_at_remote(vma_op_request_t *req)
{
	long ret = -EPERM;

	switch (req->operation) {
	case VMA_OP_MUNMAP:
		ret = vm_munmap(req->addr, req->len);
		break;
	case VMA_OP_MMAP:
	case VMA_OP_MPROTECT:
	case VMA_OP_MREMAP:
	case VMA_OP_BRK:
	case VMA_OP_MADVISE:
		BUG_ON("Not implemented yet");
		break;
	default:
		BUG_ON("unreachable");

	}
	return ret;
}

static long __process_vma_op_at_origin(vma_op_request_t *req)
{
	long ret = -EPERM;
	int from_nid = PCN_KMSG_FROM_NID(req);

	switch (req->operation) {
	case VMA_OP_MMAP: {
		unsigned long populate = 0;
		unsigned long raddr;
		struct file *f = NULL;
		struct mm_struct *mm = get_task_mm(current);

		if (req->path[0] != '\0')
			f = filp_open(req->path, O_RDONLY | O_LARGEFILE, 0);

		if (IS_ERR(f)) {
			ret = PTR_ERR(f);
			printk("  [%d] Cannot open %s %ld\n", current->pid, req->path, ret);
			mmput(mm);
			break;
		}
		down_write(&mm->mmap_sem);
		raddr = do_mmap_pgoff(f, req->addr, req->len, req->prot,
				req->flags, req->pgoff, &populate);
		up_write(&mm->mmap_sem);
		if (populate) mm_populate(raddr, populate);

		ret = IS_ERR_VALUE(raddr) ? raddr : 0;
		req->addr = raddr;
		VSPRINTK("  [%d] %lx %lx -- %lx %lx %lx\n", current->pid,
				ret, req->addr, req->addr + req->len, req->prot, req->flags);

		if (f) filp_close(f, NULL);
		mmput(mm);
		break;
	}
	case VMA_OP_BRK: {
		unsigned long brk = req->brk;
		req->brk = sys_brk(req->brk);
		ret = brk != req->brk;
		break;
	}
	case VMA_OP_MUNMAP:
		ret = vma_server_munmap_origin(req->addr, req->len, from_nid);
		break;
	case VMA_OP_MPROTECT:
		ret = sys_mprotect(req->addr, req->len, req->prot);
		break;
	case VMA_OP_MREMAP:
		ret = sys_mremap(req->addr, req->old_len, req->new_len,
			req->flags, req->new_addr);
		break;
	case VMA_OP_MADVISE:
		if (req->behavior == MADV_RELEASE) {
			ret = process_madvise_release_from_remote(
					from_nid, req->start, req->start + req->len);
		} else {
			ret = sys_madvise(req->start, req->len, req->behavior);
		}
		break;
	default:
		BUG_ON("unreachable");
	}

	return ret;
}

void process_vma_op_request(vma_op_request_t *req)
{
	long ret = 0;
	VSPRINTK("\nVMA_OP_REQUEST [%d] %s %lx %lx %lx\n", current->pid,
			vma_op_code_sz[req->operation], req->addr, req->len,
			instruction_pointer(current_pt_regs()));

	if (current->at_remote) {
		ret = __process_vma_op_at_remote(req);
	} else {
		ret = __process_vma_op_at_origin(req);
	}

	VSPRINTK("  [%d] ->%s %ld\n", current->pid,
			vma_op_code_sz[req->operation], ret);

	__reply_vma_op(req, ret);
	pcn_kmsg_done(req);
}


/**
 * Response for remote VMA request and handling the response
 */
struct vma_info {
	struct list_head list;
	unsigned long addr;
	atomic_t pendings;
	struct completion complete;
	wait_queue_head_t pendings_wait;

	volatile int ret;
	volatile vma_info_response_t *response;
};

static struct vma_info *__lookup_pending_vma_request(struct remote_context *rc, unsigned long addr)
{
	struct vma_info *vi;

	list_for_each_entry(vi, &rc->vmas, list) {
		if (vi->addr == addr) return vi;
	}
	return NULL;
}

static int handle_vma_info_response(struct pcn_kmsg_message *msg)
{
	vma_info_response_t *res = (vma_info_response_t *)msg;
	struct task_struct *tsk;
	unsigned long flags;
	struct vma_info *vi;
	struct remote_context *rc;

	tsk = __get_task_struct(res->remote_pid);
	if (WARN_ON(!tsk)) {
		goto out_free;
	}
	rc = get_task_remote(tsk);

	spin_lock_irqsave(&rc->vmas_lock, flags);
	vi = __lookup_pending_vma_request(rc, res->addr);
	spin_unlock_irqrestore(&rc->vmas_lock, flags);
	put_task_remote(tsk);
	put_task_struct(tsk);

	if (WARN_ON(!vi)) {
		goto out_free;
	}

	vi->response = res;
	complete(&vi->complete);
	return 0;

out_free:
	pcn_kmsg_done(res);
	return 0;
}


/**
 * Handle VMA info requests at the origin.
 * This is invoked through the remote work delegation.
 */
void process_vma_info_request(vma_info_request_t *req)
{
	vma_info_response_t *res = NULL;
	struct mm_struct *mm;
	struct vm_area_struct *vma;
	unsigned long addr = req->addr;

	might_sleep();

	while (!res) {
		res = kmalloc(sizeof(*res), GFP_KERNEL);
	}
	res->addr = addr;

	mm = get_task_mm(current);
	down_read(&mm->mmap_sem);

	vma = find_vma(mm, addr);
	if (unlikely(!vma)) {
		printk("vma_info: vma does not exist at %lx\n", addr);
		res->result = -ENOENT;
		goto out_up;
	}
	if (likely(vma->vm_start <= addr)) {
		goto good;
	}
	if (unlikely(!(vma->vm_flags & VM_GROWSDOWN))) {
		/* TODO Jack BUG TODO Jack BUG TODO Jack BUG TODO Jack BUG TODO Jack BUG */
		/* TODO Jack BUG TODO Jack BUG TODO Jack BUG TODO Jack BUG TODO Jack BUG */
		/* TODO Jack BUG TODO Jack BUG TODO Jack BUG TODO Jack BUG TODO Jack BUG */
		printk("vma_info: vma does not really exist at %lx\n", addr);
		res->result = -ENOENT;
		goto out_up;
	}

good:
	res->vm_start = vma->vm_start;
	res->vm_end = vma->vm_end;
	res->vm_flags = vma->vm_flags;
	res->vm_pgoff = vma->vm_pgoff;

	get_file_path(vma->vm_file, res->vm_file_path, sizeof(res->vm_file_path));
	res->result = 0;

out_up:
	up_read(&mm->mmap_sem);
	mmput(mm);

	if (res->result == 0) {
		VSPRINTK("\n## VMA_INFO [%d] %lx -- %lx %lx\n", current->pid,
				res->vm_start, res->vm_end, res->vm_flags);
		if (!vma_info_anon(res)) {
			VSPRINTK("  [%d] %s + %lx\n", current->pid,
					res->vm_file_path, res->vm_pgoff);
		}
	}

	res->remote_pid = req->remote_pid;
	pcn_kmsg_send(PCN_KMSG_TYPE_VMA_INFO_RESPONSE,
			PCN_KMSG_FROM_NID(req), res, sizeof(*res));

	pcn_kmsg_done(req);
	kfree(res);
	return;
}


static struct vma_info *__alloc_vma_info_request(struct task_struct *tsk, unsigned long addr, vma_info_request_t **preq)
{
	struct vma_info *vi = kmalloc(sizeof(*vi), GFP_KERNEL);
	vma_info_request_t *req = kmalloc(sizeof(*req), GFP_KERNEL);

	BUG_ON(!vi || !req);

	/* vma_info */
	INIT_LIST_HEAD(&vi->list);
	vi->addr = addr;
	vi->response = (volatile vma_info_response_t *)0xdeadbeaf; /* poision */
	atomic_set(&vi->pendings, 0);
	init_completion(&vi->complete);
	init_waitqueue_head(&vi->pendings_wait);

	/* req */
	req->origin_pid = tsk->origin_pid;
	req->remote_pid = tsk->pid;
	req->addr = addr;

	*preq = req;

	return vi;
}


static int __update_vma(struct task_struct *tsk, vma_info_response_t *res)
{
	struct mm_struct *mm = tsk->mm;
	struct vm_area_struct *vma;
	unsigned long prot;
	unsigned flags = MAP_FIXED;
	struct file *f = NULL;
	unsigned long err = 0;
	int ret = 0;
	unsigned long addr = res->addr;

	if (res->result) {
		down_read(&mm->mmap_sem);
		return res->result;
	}

	while (!down_write_trylock(&mm->mmap_sem)) {
		schedule();
	}
	vma = find_vma(mm, addr);
	VSPRINTK("  [%d] %lx %lx\n", tsk->pid, vma ? vma->vm_start : 0, addr);
	if (vma && vma->vm_start <= addr) {
		/* somebody already done for me. */
		goto out;
	}

	if (vma_info_anon(res)) {
		flags |= MAP_ANONYMOUS;
	} else {
		f = filp_open(res->vm_file_path, O_RDONLY | O_LARGEFILE, 0);
		if (IS_ERR(f)) {
			printk(KERN_ERR"%s: cannot find backing file %s\n",__func__,
				res->vm_file_path);
			ret = -EIO;
			goto out;
		}
		/*
		unsigned long orig_pgoff = res->vm_pgoff;
		res->vm_pgoff = __get_file_offset(f, res->vm_start);
		BUG_ON(res->vm_pgoff == -1);
		*/
		VSPRINTK("  [%d] %s + %lx\n", tsk->pid,
				res->vm_file_path, res->vm_pgoff);
	}

	prot  = ((res->vm_flags & VM_READ) ? PROT_READ : 0)
			| ((res->vm_flags & VM_WRITE) ? PROT_WRITE : 0)
			| ((res->vm_flags & VM_EXEC) ? PROT_EXEC : 0);

	flags = flags
			| ((res->vm_flags & VM_DENYWRITE) ? MAP_DENYWRITE : 0)
			| ((res->vm_flags & VM_SHARED) ? MAP_SHARED : MAP_PRIVATE)
			| ((res->vm_flags & VM_GROWSDOWN) ? MAP_GROWSDOWN : 0);

	err = map_difference(mm, f, res->vm_start, res->vm_end,
				prot, flags, res->vm_pgoff);

	if (f) filp_close(f, NULL);

	/*
	vma = find_vma(mm, addr);
	BUG_ON(!vma || vma->vm_start > addr);
	if (res->vm_flags & VM_FETCH_LOCAL) vma->vm_flags |= VM_FETCH_LOCAL;
	*/
out:
	downgrade_write(&mm->mmap_sem);
	return ret;
}


/**
 * Fetch VMA information from the origin.
 * mm->mmap_sem is down_read() at this point and should be downed upon return.
 */
int vma_server_fetch_vma(struct task_struct *tsk, unsigned long address)
{
	struct vma_info *vi;
	unsigned long flags;
	DEFINE_WAIT(wait);
	int ret = 0;
	unsigned long addr = address & PAGE_MASK;
	vma_info_request_t *req = NULL;
	struct remote_context *rc = get_task_remote(tsk);

	might_sleep();

	VSPRINTK("\n## VMAFAULT [%d] %lx %lx\n", current->pid,
			address, instruction_pointer(current_pt_regs()));

	spin_lock_irqsave(&rc->vmas_lock, flags);
	vi = __lookup_pending_vma_request(rc, addr);
	if (!vi) {
		struct vma_info *v;
		spin_unlock_irqrestore(&rc->vmas_lock, flags);

		vi = __alloc_vma_info_request(tsk, addr, &req);

		spin_lock_irqsave(&rc->vmas_lock, flags);
		v = __lookup_pending_vma_request(rc, addr);
		if (!v) {
			list_add(&vi->list, &rc->vmas);
		} else {
			kfree(vi);
			vi = v;
			kfree(req);
			req = NULL;
		}
	}
	up_read(&tsk->mm->mmap_sem);

	if (req) {
		spin_unlock_irqrestore(&rc->vmas_lock, flags);

		VSPRINTK("  [%d] %lx ->[%d/%d]\n", current->pid,
				addr, tsk->origin_pid, tsk->origin_nid);
		pcn_kmsg_send(PCN_KMSG_TYPE_VMA_INFO_REQUEST,
				tsk->origin_nid, req, sizeof(*req));
		wait_for_completion(&vi->complete);

		ret = vi->ret =
			__update_vma(tsk, (vma_info_response_t *)vi->response);

		spin_lock_irqsave(&rc->vmas_lock, flags);
		list_del(&vi->list);
		spin_unlock_irqrestore(&rc->vmas_lock, flags);

		pcn_kmsg_done((void *)vi->response);
		wake_up_all(&vi->pendings_wait);

		kfree(req);
	} else {
		VSPRINTK("  [%d] %lx already pended\n", current->pid, addr);
		atomic_inc(&vi->pendings);
		prepare_to_wait(&vi->pendings_wait, &wait, TASK_UNINTERRUPTIBLE);
		spin_unlock_irqrestore(&rc->vmas_lock, flags);

		io_schedule();
		finish_wait(&vi->pendings_wait, &wait);

		smp_rmb();
		ret = vi->ret;
		if (atomic_dec_and_test(&vi->pendings)) {
			kfree(vi);
		}
		down_read(&tsk->mm->mmap_sem);
	}

	put_task_remote(tsk);
	return ret;
}


DEFINE_KMSG_RW_HANDLER(vma_info_request, vma_info_request_t, origin_pid);
DEFINE_KMSG_RW_HANDLER(vma_op_request, vma_op_request_t, origin_pid);

int vma_server_init(void)
{
	REGISTER_KMSG_HANDLER(
			PCN_KMSG_TYPE_VMA_INFO_REQUEST, vma_info_request);
	REGISTER_KMSG_HANDLER(
			PCN_KMSG_TYPE_VMA_INFO_RESPONSE, vma_info_response);

	REGISTER_KMSG_HANDLER(PCN_KMSG_TYPE_VMA_OP_REQUEST, vma_op_request);
	REGISTER_KMSG_HANDLER(PCN_KMSG_TYPE_VMA_OP_RESPONSE, vma_op_response);

	return 0;
}
