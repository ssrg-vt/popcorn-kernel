#ifndef ___POPCORN_PCIE_H__
#define ___POPCORN_PCIE_H__



/* Xilinx Vendor ID and Device ID programmed by Vivado Design Suite
   Can be changed according to the device specifications */

#define VEND_ID 0x10EE
#define DEV_ID 0x903F

// Region of interest for this application.
//If PCIe to AXI Lite Master Interface is enabled then this region/BAR would change to 2.

#define XDMA_MSB_MASK 0xFFFFFFFF00000000LL
#define XDMA_LSB_MASK 0xFFFFFFFFLL

#define XDMA_SIZE 0x10000
#define AXI_SIZE 0x100000

/* Offset Addresses */

#define desc_byp1 0x0000
#define desc_byp2 0x10000

#define xxv_0 0x40000
#define xxv_1 0x80000
#define xxv_2 0xC0000

#define dsm_proc 0x20000

#define switch_ctl 0x80000

/* Register Offsets of Xilinx XDMA */

#define h2c_ctl 0x04
#define c2h_ctl 0x1004
#define h2c_ch 0x40
#define c2h_ch 0x1040
#define h2c_stat 0x44
#define c2h_stat 0x1044
#define h2cintr 0x90
#define c2hintr 0x1090

#define h2c1_ctl 0x104
#define c2h1_ctl 0x1104
#define h2c1_ch 0x140
#define c2h1_ch 0x1140
#define h2c1_stat 0x144
#define c2h1_stat 0x1144
#define h2c1intr 0x190
#define c2h1intr 0x1190

#define sgdma 0x6010

#define usr_irq 0x2040
#define ch_irq 0x2044

#define usr_irqen 0x2004
#define ch_irqen 0x2010

#define usr_irq_mask 0x200C
#define usr_irq_enable 0x2008
#define ch_irq_mask 0x2018
#define ch_irq_enable 0x2014

#define ch_irq_pending 0x204C
#define usr_irq_pending 0x2048

#define h2c_poll_wr_msb 0x8C
#define h2c_poll_wr_lsb 0x88
#define c2h_poll_wr_msb 0x108C
#define c2h_poll_wr_lsb 0x1088


/* Register Offsets of Ethernet Subsystem */

#define xxv_rxen xxv_0 + 0x014
#define xxv_txen xxv_0 + 0x00C

#define xxv1_rxen xxv_1 + 0x014
#define xxv1_txen xxv_1 + 0x00C

#define xxv2_rxen xxv_2 + 0x014
#define xxv2_txen xxv_2 + 0x00C

#define xxv_reset xxv_0 + 0x04
#define xxv1_reset xxv_1 + 0x04
#define xxv2_reset xxv_2 + 0x04

/* H2C Descriptor - Channel 0 Bypass Configuration */

#define Ctl1 desc_byp1 + 0x00
#define Control1 desc_byp1 + 0x1C
#define DA1 desc_byp1 + 0x04
#define SA1 desc_byp1 + 0x0C
#define length1 desc_byp1 + 0x14
#define N1 desc_byp1 + 0x18

/* C2H Descriptor - Channel 0 Bypass Configuration */

#define Ctl2 desc_byp2 + 0x00
#define Control2 desc_byp2 + 0x1C
#define DA2 desc_byp2 + 0x04
#define SA2 desc_byp2 + 0x0C
#define length2 desc_byp2 + 0x14
#define N2 desc_byp2 + 0x18

/* H2C Descriptor - Channel 1 Bypass Configuration 

#define Ctl3 desc_byp3 + 0x00
#define Control3 desc_byp3 + 0x1C
#define DA3 desc_byp3 + 0x04
#define SA3 desc_byp3 + 0x0C
#define length3 desc_byp3 + 0x14
#define N3 desc_byp3 + 0x18

/* C2H Descriptor - Channel 1 Bypass Configuration 

#define Ctl4 desc_byp4 + 0x00
#define Control4 desc_byp4 + 0x1C
#define DA4 desc_byp4 + 0x04
#define SA4 desc_byp4 + 0x0C
#define length4 desc_byp4 + 0x14
#define N4 desc_byp4 + 0x18

/* DSM Protocol Processor Configuration */

//Control Regs

#define proc_pkey_msb dsm_proc + 0x00
#define proc_pkey_lsb dsm_proc + 0x78
#define proc_vaddr_msb dsm_proc + 0x04
#define proc_vaddr_lsb dsm_proc + 0x08
#define proc_daddr_msb dsm_proc + 0x0C
#define proc_daddr_lsb dsm_proc + 0x10
#define proc_vm_result dsm_proc + 0x6C
#define rpr_type dsm_proc + 0x70
#define proc_fflags_msb dsm_proc + 0x14
#define proc_fflags_lsb dsm_proc + 0x18
#define proc_iaddr_msb dsm_proc + 0x1C
#define proc_iaddr_lsb dsm_proc + 0x20
#define proc_ws_id dsm_proc + 0x24
#define proc_rpid dsm_proc + 0x28
#define proc_opid dsm_proc + 0x2C
#define proc_nid dsm_proc + 0x30
#define proc_mynid dsm_proc + 0x34
#define proc_ctl dsm_proc + 0x38
#define proc_resp_type dsm_proc + 0x88
#define proc_mask dsm_proc + 0x7C

//Writeback Regs from processor to read

#define wr_fflags_msb dsm_proc + 0x3C
#define wr_fflags_lsb dsm_proc + 0x40
#define wr_vaddr_msb dsm_proc + 0x44
#define wr_vaddr_lsb dsm_proc + 0x48
#define wr_iaddr_msb dsm_proc + 0x4C
#define wr_iaddr_lsb dsm_proc + 0x50
#define wr_pkey_msb dsm_proc + 0x54
#define wr_pkey_lsb dsm_proc + 0x74
#define wr_wsid dsm_proc + 0x58
#define wr_rpid dsm_proc + 0x5C
#define wr_opid dsm_proc + 0x60
#define wr_nid dsm_proc + 0x64
#define wr_daddr_lsb dsm_proc + 0x68
#define wr_page_resp dsm_proc + 0x80
#define wr_vm_res dsm_proc + 0x84


/* Switch Control Regs */

#define thresh 4096

#define RPR_WR 0xCCCC
#define RPR_RD 0xBBBB
#define VMF_CONTINUE 0xAAAA
#define INVALIDATE 0xFFFF
#define C2H_MAX_SIZE 5000
#define FDSM_MSG_SIZE 8192

void write_register(u32 value, void *iomem);
inline u32 read_register(void *iomem);
int init_pcie_xdma(struct pci_dev *pci_dev, void __iomem *p, void __iomem *g);
void write_mynid(int nid);

int xdma_transfer(int y);
int config_descriptors_bypass(dma_addr_t dma_addr, size_t size, int y, int z);
void channel_interrupts_disable(int z, int x);
void user_interrupts_disable(int x);
void channel_interrupts_enable(int z, int x);
void user_interrupts_enable(int x);
void __iomem * return_iomaps(int x);


/* Prot_Proc Functions */

void prot_proc_handle_localfault(unsigned long vmf, unsigned long vaddr, unsigned long iaddr, 
	unsigned long pkey, pid_t opid, pid_t rpid, int nid, unsigned long fflags, int ws_id, int tsk_remote);
void * prot_proc_handle_rpr(int x);
void * prot_proc_handle_inval(void);
void xdma_post_response(enum pcn_kmsg_type type, int result, int from_nid, unsigned long vaddr, pid_t rpid, pid_t opid, 
	int ws_id, unsigned long pkey);

unsigned long current_pkey(void);
void resolve_waiting(int ws_id);
void pending(void);

#endif
