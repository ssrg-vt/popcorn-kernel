/*Need to update this file for my design.*/
#include <linux/module.h>
#include <linux/kernel.h>
#include <linux/slab.h>
#include <linux/delay.h>
#include <linux/timekeeping.h>
#include <linux/spinlock.h>
#include <asm/io.h>
#include <popcorn/stat.h>
#include <popcorn/debug.h>
#include <popcorn/bundle.h>
#include <popcorn/pcn_kmsg.h>
#include <popcorn/page_server.h>
#include <popcorn/pcie.h>

#include "wait_station.h"
#include "types.h"

void __iomem *xdma_axi;
void __iomem *xdma_ctl;

static DEFINE_SPINLOCK(prot_proc_lock);
static DEFINE_SPINLOCK(xdma_lock);

u64 sstart_time, eend_time, rres_time; 

enum {
	AXI = 0,
	CTL = 1,
};

enum {
	KMSG = 0,
	PAGE = 1,
	RPR_READ = 2,
	INVAL = 3,
	FAULT = 4,
	MKWRITE = 5,
	RESP = 6,
	RPR_WRITE = 7,
	VMFC = 8,
};

enum {
	PGREAD = 0,
	PGWRITE = 1,
	VMFCON = 2,
	PGINVAL = 3,
	PGRESP = 4,
};

void write_register(u32 value, void *iomem)
{
	iowrite32(value, iomem);
}
EXPORT_SYMBOL(write_register);

/* SNIC-DSM Functions */

void resolve_waiting(int ws_id)
{
	struct wait_station *ws;
	ws = wait_station(ws_id);

	if (atomic_dec_and_test(&ws->pendings_count)) {
		complete(&ws->pendings);
	}
}
EXPORT_SYMBOL(resolve_waiting);

/* Local Fault Handler */

void prot_proc_handle_localfault(unsigned long vmf, unsigned long vaddr, unsigned long iaddr, unsigned long pkey, 
	pid_t opid, pid_t rpid, int from_nid, unsigned long fflags, int ws_id, int tsk_remote)
{	    
		spin_lock(&prot_proc_lock);
		write_register((u32)((vaddr & XDMA_MSB_MASK) >> 32), (u32 *)(xdma_axi + proc_vaddr_msb));
		write_register((u32)(vaddr & XDMA_LSB_MASK), (u32 *)(xdma_axi + proc_vaddr_lsb));
		write_register((u32)((fflags & XDMA_MSB_MASK) >> 32), (u32 *)(xdma_axi + proc_fflags_msb));
		write_register((u32)(fflags & XDMA_LSB_MASK), (u32 *)(xdma_axi + proc_fflags_lsb));
		write_register((u32)((iaddr & XDMA_MSB_MASK) >> 32), (u32 *)(xdma_axi + proc_iaddr_msb));
		write_register((u32)(iaddr & XDMA_LSB_MASK), (u32 *)(xdma_axi + proc_iaddr_lsb));
		if (pkey){
			write_register((u32)((pkey & XDMA_MSB_MASK) >> 32), (u32 *)(xdma_axi + proc_pkey_msb));
			write_register((u32)(pkey & XDMA_LSB_MASK), (u32 *)(xdma_axi + proc_pkey_lsb));
		} else {
			write_register(0x00000000, (u32 *)(xdma_axi + proc_pkey_msb));
			write_register(0x00000000, (u32 *)(xdma_axi + proc_pkey_lsb));
		}	
		write_register(ws_id, (u32 *)(xdma_axi + proc_ws_id));
		write_register(opid, (u32 *)(xdma_axi + proc_opid));
		write_register(rpid, (u32 *)(xdma_axi + proc_rpid));
		write_register(from_nid, (u32 *)(xdma_axi + proc_nid));
 		if (tsk_remote) {
 			write_register(0x8001, (u32 *)(xdma_axi + proc_ctl));
 		} else {
 			write_register(0x01, (u32 *)(xdma_axi + proc_ctl));
 		}
 		write_register(0x00, (u32 *)(xdma_axi + proc_ctl));
 		spin_unlock(&prot_proc_lock);
}
EXPORT_SYMBOL(prot_proc_handle_localfault);

/* Remote Page Request Handler */

void * prot_proc_handle_rpr(int x)
{
	remote_page_request_t *req = pcn_kmsg_get(sizeof(*req));
	req->origin_ws = (int)ioread32((u32 *)(xdma_axi + wr_wsid));
	req->remote_pid = (pid_t)ioread32((u32 *)(xdma_axi + wr_rpid));
	req->origin_pid = (pid_t)ioread32((u32 *)(xdma_axi + wr_opid));
	req->addr = ((unsigned long long) ioread32((u32 *)(xdma_axi + wr_vaddr_msb)) << 32 | ioread32((u32 *)(xdma_axi + wr_vaddr_lsb)));
	req->from_nid = (int)ioread32((u32 *)(xdma_axi + wr_nid));
	req->instr_addr = ((unsigned long long) ioread32((u32 *)(xdma_axi + wr_iaddr_msb)) << 32 | ioread32((u32 *)(xdma_axi + wr_iaddr_lsb)));
	req->fault_flags = ((unsigned long long) ioread32((u32 *)(xdma_axi + wr_fflags_msb)) << 32 | ioread32((u32 *)(xdma_axi + wr_fflags_lsb)));
	req->pkey = ((unsigned long long) ioread32((u32 *)(xdma_axi + wr_pkey_msb)) << 32 | ioread32((u32 *)(xdma_axi + wr_pkey_lsb)));
	req->type = x;
	return req;

}
EXPORT_SYMBOL(prot_proc_handle_rpr);

/* Invalidate Page Request Handler */

void * prot_proc_handle_inval()
{
	page_invalidate_request_t *req = pcn_kmsg_get(sizeof(*req));
	req->origin_ws = (int)ioread32((u32 *)(xdma_axi + wr_wsid));
	req->remote_pid = (pid_t)ioread32((u32 *)(xdma_axi + wr_rpid));
	req->origin_pid = (pid_t)ioread32((u32 *)(xdma_axi + wr_opid));
	req->from_nid = (int)ioread32((u32 *)(xdma_axi + wr_nid));
	req->addr = ((unsigned long long) ioread32((u32 *)(xdma_axi + wr_vaddr_msb)) << 32 | ioread32((u32 *)(xdma_axi + wr_vaddr_lsb)));
	req->pkey = ((unsigned long long) ioread32((u32 *)(xdma_axi + wr_pkey_msb)) << 32 | ioread32((u32 *)(xdma_axi + wr_pkey_lsb)));
	return req;
}
EXPORT_SYMBOL(prot_proc_handle_inval);

/* Init Functions */

void write_mynid(int nid)
{
	write_register(nid, (u32 *) (xdma_axi + proc_mynid));
}
EXPORT_SYMBOL(write_mynid);

unsigned long current_pkey()
{
	unsigned long pkey;
	pkey = ((unsigned long) ioread32((u32 *)(xdma_axi + wr_pkey_msb)) << 32 | ioread32((u32 *)(xdma_axi + wr_pkey_lsb)));
	return pkey;
}
EXPORT_SYMBOL(current_pkey);

void __iomem * return_iomaps(int x)
{
	if (!x) {
		return xdma_axi;
	} else {
		return xdma_ctl;
	}
}
EXPORT_SYMBOL(return_iomaps);

/* PCIe Initialization Handler */

int init_axi(void __iomem *g)//, void __iomem *p)
{
	xdma_axi = g;
	return 0;
}
EXPORT_SYMBOL(init_axi);
