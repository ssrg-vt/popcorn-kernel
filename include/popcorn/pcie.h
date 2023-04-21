#ifndef ___POPCORN_PCIE_H__
#define ___POPCORN_PCIE_H__


#define XDMA_MSB_MASK 0xFFFFFFFF00000000LL
#define XDMA_LSB_MASK 0x00000000FFFFFFFFLL
#define dsm_proc 0x00800000 //Setting the 23rd bit to indicate the writes are for prot proc

/* DSM Protocol Processor Configuration */

//Control Regs
//Need to left shift the offset bits by 2 to make up for the AT field in the tdata signal.
#define proc_pkey_msb dsm_proc + (0x00 << 2) //0
#define proc_pkey_lsb dsm_proc + (0x78 << 2) //1e0
#define proc_vaddr_msb dsm_proc + (0x04 << 2) //10
#define proc_vaddr_lsb dsm_proc + (0x08 << 2) //20
#define proc_daddr_msb dsm_proc + (0x0C << 2) //30
#define proc_daddr_lsb dsm_proc + (0x10 << 2) //40
#define proc_vm_result dsm_proc + (0x6C << 2) //1b0
#define rpr_type dsm_proc + (0x70 << 2) //1c0
#define proc_fflags_msb dsm_proc + (0x14 << 2)//50
#define proc_fflags_lsb dsm_proc + (0x18 << 2)//60
#define proc_iaddr_msb dsm_proc + (0x1C << 2)//70
#define proc_iaddr_lsb dsm_proc + (0x20 << 2)//80
#define proc_ws_id dsm_proc + (0x24 << 2)//90
#define proc_rpid dsm_proc + (0x28 << 2)//a0
#define proc_opid dsm_proc + (0x2C << 2)//b0
#define proc_nid dsm_proc + (0x30 << 2)//c0
#define proc_mynid dsm_proc + (0x34 << 2)//d0
#define proc_ctl dsm_proc + (0x38 << 2)//e0
#define proc_resp_type dsm_proc + (0x88 << 2)//220
#define proc_mask dsm_proc + (0x7C << 2)//1f0

//Writeback Regs from processor to read

#define wr_fflags_msb dsm_proc + (0x3C << 2)//f0
#define wr_fflags_lsb dsm_proc + (0x40 << 2)//100
#define wr_vaddr_msb dsm_proc + (0x44 << 2)//110
#define wr_vaddr_lsb dsm_proc + (0x48 << 2)//120
#define wr_iaddr_msb dsm_proc + (0x4C << 2)//130
#define wr_iaddr_lsb dsm_proc + (0x50 << 2)//140
#define wr_pkey_msb dsm_proc + (0x54 << 2)//150
#define wr_pkey_lsb dsm_proc + (0x74 << 2)//1d0
#define wr_wsid dsm_proc + (0x58 << 2)//160
#define wr_rpid dsm_proc + (0x5C << 2)//170
#define wr_opid dsm_proc + (0x60 << 2)//180
#define wr_nid dsm_proc + (0x64 << 2)//190
#define wr_daddr_lsb dsm_proc + (0x68 << 2)//1a0
#define wr_page_resp dsm_proc + (0x80 << 2)//200
#define wr_vm_res dsm_proc + (0x84 << 2)//210


/* Switch Control Regs */

#define thresh 4096

#define RPR_WR 0xCCCC
#define RPR_RD 0xBBBB
#define VMF_CONTINUE 0xAAAA
#define INVALIDATE 0xFFFF
#define C2H_MAX_SIZE 5000
#define FDSM_MSG_SIZE 8192

/*Might have to remove some functions*/
void write_register(u64 value, void *iomem);
//inline u32 read_register(void *iomem);
int init_pcie(struct pci_dev *pci_dev, void __iomem *g);//, void __iomem *g);
void write_mynid(int nid);

//int xdma_transfer(int y);
//int config_descriptors_bypass(dma_addr_t dma_addr, size_t size, int y, int z);
//void channel_interrupts_disable(int z, int x);
//void user_interrupts_disable(int x);
//void channel_interrupts_enable(int z, int x);
//void user_interrupts_enable(int x);
//void __iomem * return_iomaps(int x);


/* Prot_Proc Functions */

void prot_proc_handle_localfault(unsigned long vmf, unsigned long vaddr, unsigned long iaddr, 
	unsigned long pkey, pid_t opid, pid_t rpid, int nid, unsigned long fflags, int ws_id, int tsk_remote);
void * prot_proc_handle_rpr(int x);
void * prot_proc_handle_inval(void);
void pcie_axi_post_response(enum pcn_kmsg_type type, int result, int from_nid, unsigned long vaddr, pid_t rpid, pid_t opid, 
	int ws_id, unsigned long pkey);

unsigned long current_pkey(void);
void resolve_waiting(int ws_id);
void pending(void);

#endif
