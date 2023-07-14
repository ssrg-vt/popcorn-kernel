#ifndef ___POPCORN_PCIE_H__
#define ___POPCORN_PCIE_H__


#define XDMA_MSB_MASK 0xFFFFFFFF00000000LL
#define XDMA_LSB_MASK 0xFFFFFFFFLL
#define dsm_proc 0x00000000

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

/*Might have to remove some functions*/
void write_register(u32 value, void *iomem);
int init_axi(void __iomem *g);
void write_mynid(int nid);

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
