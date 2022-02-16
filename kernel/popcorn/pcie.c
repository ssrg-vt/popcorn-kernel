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

u32 read_register(void *iomem)
{
	return ioread32(iomem);
}
EXPORT_SYMBOL(read_register);

void init_descriptor_bypass(void)
{
	write_register(0x13, xdma_axi + Ctl1);
	write_register(1, xdma_axi + N1);
	write_register(0x13, xdma_axi + Ctl2);
	write_register(1, xdma_axi + N2);
}
EXPORT_SYMBOL(init_descriptor_bypass);

int init_xdma(void)
{
	/* Resetting the XDMA */

	write_register(0, xdma_ctl + h2c_ctl);
	write_register(0, xdma_ctl + c2h_ctl);

	/* Configuring the Interrupt Enable Masks */

	write_register(0x10001, xdma_ctl + sgdma);
	write_register(0x00, xdma_ctl + ch_irqen);
	write_register(0x03, xdma_ctl + usr_irqen);

	return (read_register(xdma_ctl + h2c_ctl) ||
		read_register(xdma_ctl + c2h_ctl));

}
EXPORT_SYMBOL(init_xdma);

void init_xxv(void)
{
	/* Enabling the RX and TX in the Ethernet Subsystem */

	write_register(0x01, xdma_axi + xxv_rxen);
	write_register(0x10, xdma_axi + xxv_txen);

	msleep(100);
	write_register(0, xdma_axi + xxv_txen);
	msleep(100);
	write_register(0x01, xdma_axi + xxv_txen);
}
EXPORT_SYMBOL(init_xxv);

/* Interrupt handling functions */

void channel_interrupts_disable(int z, int x)
{
	if (z) {
		if (!x) {

			write_register(0x00,  (u32 *)(xdma_ctl + h2c_ctl));
			write_register(0x01, (u32 *)(xdma_ctl + ch_irq_mask));
			read_register((u32 *)(xdma_ctl + h2c_stat));
		} 
	} else {
		if (!x) {
		    write_register(0x00, (u32 *)(xdma_ctl + c2h_ctl));
		    write_register(0x02, (u32 *)(xdma_ctl + ch_irq_mask));
			read_register((u32 *)(xdma_ctl + c2h_stat));
		}
	}
}
EXPORT_SYMBOL(channel_interrupts_disable);

void channel_interrupts_enable(int z, int x)
{
	 if (z) {
		if (!x) {
			write_register(ioread32((u32 *)(xdma_ctl + ch_irq_enable)) | 0x01, (u32 *)(xdma_ctl + ch_irq_enable));
		}
	} else {
		if (!x) {
			write_register(ioread32((u32 *)(xdma_ctl + ch_irq_enable)) | 0x02, (u32 *)(xdma_ctl + ch_irq_enable));

		}
	} 
}
EXPORT_SYMBOL(channel_interrupts_enable);

void user_interrupts_disable(int x)
{
	if (!x) {
		write_register(0x01, (u32 *)(xdma_ctl + usr_irq_mask));		
	} else if (x == FAULT){
		write_register(0x02, (u32 *)(xdma_ctl + usr_irq_mask));
		write_register(0x00, (u32 *)(xdma_axi + proc_ctl));
		write_register(0x08, (u32 *)(xdma_axi + proc_mask));
	} else {
		PCNPRINTK("Something wrong with the user_interrupts_disable\n");
	}
}
EXPORT_SYMBOL(user_interrupts_disable);

void user_interrupts_enable(int x)
{
	 if (!x) {
		write_register(0x03, (u32 *)(xdma_ctl + usr_irq_enable));
	} else if (x == FAULT) {
		write_register(ioread32((u32 *)(xdma_ctl + usr_irq_enable)) | 0x02, (u32 *)(xdma_ctl + usr_irq_enable));
		write_register(0x00, (u32 *)(xdma_axi + proc_mask));
	} else {
		PCNPRINTK("Something wrong with the user_interrupts_enable\n");
	}
}
EXPORT_SYMBOL(user_interrupts_enable);

/* Configure descriptor bypass */

int config_descriptors_bypass(dma_addr_t dma_addr, size_t size, int y, int z)
{
	u32 addr_msb, addr_lsb;

	addr_msb = (u32)((dma_addr & XDMA_MSB_MASK) >> 32);
	addr_lsb = (u32)(dma_addr & XDMA_LSB_MASK);
	if (y){
		if (!z) {
			write_register(addr_msb, xdma_axi + SA1);
			write_register(addr_lsb, xdma_axi + SA1 + 0x04);
			if (size < thresh) {
				size = thresh;
			}
			write_register(size, xdma_axi + length1);
			return 0;
		}
	}  else {
		if (!z) {
			write_register(addr_msb, xdma_axi + DA2);
			write_register(addr_lsb, xdma_axi + DA2 + 0x04);
			write_register(size, xdma_axi + length2);
			return 0;

		}
	}
}
EXPORT_SYMBOL(config_descriptors_bypass);

/* Transfer to FPGA */

int xdma_transfer(int y)
{
	if (y){
			write_register(0x4FFFE25, xdma_ctl + h2c_ctl);
			write_register(0x01, xdma_axi + Control1);
			return 0;

	} else {
			write_register(0x4FFFE25, xdma_ctl + c2h_ctl);
			write_register(0x01, xdma_axi + Control2);
			return 0;
	}
}
EXPORT_SYMBOL(xdma_transfer);

/* fDSM Functions */

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

int init_pcie_xdma(struct pci_dev *pci_dev, void __iomem *p, void __iomem *g)
{
	int ret;

	xdma_ctl = p;
	xdma_axi = g;
	if (init_xdma()) {
		return 1;
	}
	init_xxv();
	init_descriptor_bypass();
	return 0;
}
EXPORT_SYMBOL(init_pcie_xdma);



/* Deprecated Functions */

void pending()
{
	unsigned long read_ch_irq, read_usr_irq, read_usr_pend, read_ch_pend;
	
	read_ch_irq = read_register(xdma_ctl + ch_irq);
	read_usr_irq = read_register(xdma_ctl + usr_irq);
	read_ch_pend = read_register(xdma_ctl + ch_irq_pending);
	read_usr_pend = read_register(xdma_ctl + usr_irq_pending);
}
EXPORT_SYMBOL(pending);


void xdma_post_response(enum pcn_kmsg_type type, int result, int from_nid, unsigned long vaddr, pid_t rpid, pid_t opid, 
	int ws_id, unsigned long pkey)
{
	spin_lock(&prot_proc_lock);
	write_register((u32)((vaddr & XDMA_MSB_MASK) >> 32), (u32 *)(xdma_axi + proc_vaddr_msb));
	write_register((u32)(vaddr & XDMA_LSB_MASK), (u32 *)(xdma_axi + proc_vaddr_lsb));
	write_register((u32)((pkey & XDMA_MSB_MASK) >> 32), (u32 *)(xdma_axi + proc_pkey_msb));
	write_register((u32)(pkey & XDMA_LSB_MASK), (u32 *)(xdma_axi + proc_pkey_lsb));
	write_register(ws_id, (u32 *)(xdma_axi + proc_ws_id));
	write_register(opid, (u32 *)(xdma_axi + proc_opid));
	write_register(rpid, (u32 *)(xdma_axi + proc_rpid));
	write_register(from_nid, (u32 *)(xdma_axi + proc_nid));
	write_register(type, (u32 *)(xdma_axi + proc_resp_type));
	write_register(result, (u32 *)(xdma_axi + proc_vm_result));
	write_register(0x80001, (u32 *)(xdma_axi + proc_ctl));
	write_register(0x00, (u32 *)(xdma_axi + proc_ctl));
	spin_unlock(&prot_proc_lock);
}
EXPORT_SYMBOL(xdma_post_response);
