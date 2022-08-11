// Copyright 1986-2020 Xilinx, Inc. All Rights Reserved.
// --------------------------------------------------------------------------------
// Tool Version: Vivado v.2020.2 (lin64) Build 3064766 Wed Nov 18 09:12:47 MST 2020
// Date        : Thu Mar  3 14:52:47 2022
// Host        : hemanthr-ssrg running 64-bit Ubuntu 18.04.6 LTS
// Command     : write_verilog -force -mode synth_stub
//               /home/hemanthr/corundum_copy/fpga/mqnic/250S0C/fpga_pcie/fpga/fpga.gen/sources_1/bd/design_2/ip/design_2_xdma_0_0/design_2_xdma_0_0_stub.v
// Design      : design_2_xdma_0_0
// Purpose     : Stub declaration of top-level module interface
// Device      : xczu19eg-ffvd1760-2-e
// --------------------------------------------------------------------------------

// This empty module with port declaration file causes synthesis tools to infer a black box for IP.
// The synthesis directives are for Synopsys Synplify support to prevent IO buffer insertion.
// Please paste the declaration into a Verilog source file or add the file as an additional source.
(* X_CORE_INFO = "design_2_xdma_0_0_core_top,Vivado 2020.2" *)
module design_2_xdma_0_0(sys_clk, sys_clk_gt, sys_rst_n, 
  cfg_ltssm_state, user_lnk_up, pci_exp_txp, pci_exp_txn, pci_exp_rxp, pci_exp_rxn, axi_aclk, 
  axi_aresetn, axi_ctl_aresetn, usr_irq_req, usr_irq_ack, msi_enable, msi_vector_width, 
  s_axil_awaddr, s_axil_awprot, s_axil_awvalid, s_axil_awready, s_axil_wdata, s_axil_wstrb, 
  s_axil_wvalid, s_axil_wready, s_axil_bvalid, s_axil_bresp, s_axil_bready, s_axil_araddr, 
  s_axil_arprot, s_axil_arvalid, s_axil_arready, s_axil_rdata, s_axil_rresp, s_axil_rvalid, 
  s_axil_rready, interrupt_out, s_axib_awid, s_axib_awaddr, s_axib_awregion, s_axib_awlen, 
  s_axib_awsize, s_axib_awburst, s_axib_awvalid, s_axib_wdata, s_axib_wstrb, s_axib_wlast, 
  s_axib_wvalid, s_axib_bready, s_axib_arid, s_axib_araddr, s_axib_arregion, s_axib_arlen, 
  s_axib_arsize, s_axib_arburst, s_axib_arvalid, s_axib_rready, s_axib_awready, 
  s_axib_wready, s_axib_bid, s_axib_bresp, s_axib_bvalid, s_axib_arready, s_axib_rid, 
  s_axib_rdata, s_axib_rresp, s_axib_rlast, s_axib_rvalid)
/* synthesis syn_black_box black_box_pad_pin="sys_clk,sys_clk_gt,sys_rst_n,cfg_ltssm_state[5:0],user_lnk_up,pci_exp_txp[15:0],pci_exp_txn[15:0],pci_exp_rxp[15:0],pci_exp_rxn[15:0],axi_aclk,axi_aresetn,axi_ctl_aresetn,usr_irq_req[0:0],usr_irq_ack[0:0],msi_enable,msi_vector_width[2:0],s_axil_awaddr[31:0],s_axil_awprot[2:0],s_axil_awvalid,s_axil_awready,s_axil_wdata[31:0],s_axil_wstrb[3:0],s_axil_wvalid,s_axil_wready,s_axil_bvalid,s_axil_bresp[1:0],s_axil_bready,s_axil_araddr[31:0],s_axil_arprot[2:0],s_axil_arvalid,s_axil_arready,s_axil_rdata[31:0],s_axil_rresp[1:0],s_axil_rvalid,s_axil_rready,interrupt_out,s_axib_awid[3:0],s_axib_awaddr[31:0],s_axib_awregion[3:0],s_axib_awlen[7:0],s_axib_awsize[2:0],s_axib_awburst[1:0],s_axib_awvalid,s_axib_wdata[511:0],s_axib_wstrb[63:0],s_axib_wlast,s_axib_wvalid,s_axib_bready,s_axib_arid[3:0],s_axib_araddr[31:0],s_axib_arregion[3:0],s_axib_arlen[7:0],s_axib_arsize[2:0],s_axib_arburst[1:0],s_axib_arvalid,s_axib_rready,s_axib_awready,s_axib_wready,s_axib_bid[3:0],s_axib_bresp[1:0],s_axib_bvalid,s_axib_arready,s_axib_rid[3:0],s_axib_rdata[511:0],s_axib_rresp[1:0],s_axib_rlast,s_axib_rvalid" */;
  input sys_clk;
  input sys_clk_gt;
  input sys_rst_n;
  output [5:0]cfg_ltssm_state;
  output user_lnk_up;
  output [15:0]pci_exp_txp;
  output [15:0]pci_exp_txn;
  input [15:0]pci_exp_rxp;
  input [15:0]pci_exp_rxn;
  output axi_aclk;
  output axi_aresetn;
  output axi_ctl_aresetn;
  input [0:0]usr_irq_req;
  output [0:0]usr_irq_ack;
  output msi_enable;
  output [2:0]msi_vector_width;
  input [31:0]s_axil_awaddr;
  input [2:0]s_axil_awprot;
  input s_axil_awvalid;
  output s_axil_awready;
  input [31:0]s_axil_wdata;
  input [3:0]s_axil_wstrb;
  input s_axil_wvalid;
  output s_axil_wready;
  output s_axil_bvalid;
  output [1:0]s_axil_bresp;
  input s_axil_bready;
  input [31:0]s_axil_araddr;
  input [2:0]s_axil_arprot;
  input s_axil_arvalid;
  output s_axil_arready;
  output [31:0]s_axil_rdata;
  output [1:0]s_axil_rresp;
  output s_axil_rvalid;
  input s_axil_rready;
  output interrupt_out;
  input [3:0]s_axib_awid;
  input [31:0]s_axib_awaddr;
  input [3:0]s_axib_awregion;
  input [7:0]s_axib_awlen;
  input [2:0]s_axib_awsize;
  input [1:0]s_axib_awburst;
  input s_axib_awvalid;
  input [511:0]s_axib_wdata;
  input [63:0]s_axib_wstrb;
  input s_axib_wlast;
  input s_axib_wvalid;
  input s_axib_bready;
  input [3:0]s_axib_arid;
  input [31:0]s_axib_araddr;
  input [3:0]s_axib_arregion;
  input [7:0]s_axib_arlen;
  input [2:0]s_axib_arsize;
  input [1:0]s_axib_arburst;
  input s_axib_arvalid;
  input s_axib_rready;
  output s_axib_awready;
  output s_axib_wready;
  output [3:0]s_axib_bid;
  output [1:0]s_axib_bresp;
  output s_axib_bvalid;
  output s_axib_arready;
  output [3:0]s_axib_rid;
  output [511:0]s_axib_rdata;
  output [1:0]s_axib_rresp;
  output s_axib_rlast;
  output s_axib_rvalid;
endmodule
