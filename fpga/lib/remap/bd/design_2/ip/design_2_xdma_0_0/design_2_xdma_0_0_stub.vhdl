-- Copyright 1986-2020 Xilinx, Inc. All Rights Reserved.
-- --------------------------------------------------------------------------------
-- Tool Version: Vivado v.2020.2 (lin64) Build 3064766 Wed Nov 18 09:12:47 MST 2020
-- Date        : Thu Mar  3 14:52:47 2022
-- Host        : hemanthr-ssrg running 64-bit Ubuntu 18.04.6 LTS
-- Command     : write_vhdl -force -mode synth_stub
--               /home/hemanthr/corundum_copy/fpga/mqnic/250S0C/fpga_pcie/fpga/fpga.gen/sources_1/bd/design_2/ip/design_2_xdma_0_0/design_2_xdma_0_0_stub.vhdl
-- Design      : design_2_xdma_0_0
-- Purpose     : Stub declaration of top-level module interface
-- Device      : xczu19eg-ffvd1760-2-e
-- --------------------------------------------------------------------------------
library IEEE;
use IEEE.STD_LOGIC_1164.ALL;

entity design_2_xdma_0_0 is
  Port ( 
    sys_clk : in STD_LOGIC;
    sys_clk_gt : in STD_LOGIC;
    sys_rst_n : in STD_LOGIC;
    cfg_ltssm_state : out STD_LOGIC_VECTOR ( 5 downto 0 );
    user_lnk_up : out STD_LOGIC;
    pci_exp_txp : out STD_LOGIC_VECTOR ( 15 downto 0 );
    pci_exp_txn : out STD_LOGIC_VECTOR ( 15 downto 0 );
    pci_exp_rxp : in STD_LOGIC_VECTOR ( 15 downto 0 );
    pci_exp_rxn : in STD_LOGIC_VECTOR ( 15 downto 0 );
    axi_aclk : out STD_LOGIC;
    axi_aresetn : out STD_LOGIC;
    axi_ctl_aresetn : out STD_LOGIC;
    usr_irq_req : in STD_LOGIC_VECTOR ( 0 to 0 );
    usr_irq_ack : out STD_LOGIC_VECTOR ( 0 to 0 );
    msi_enable : out STD_LOGIC;
    msi_vector_width : out STD_LOGIC_VECTOR ( 2 downto 0 );
    s_axil_awaddr : in STD_LOGIC_VECTOR ( 31 downto 0 );
    s_axil_awprot : in STD_LOGIC_VECTOR ( 2 downto 0 );
    s_axil_awvalid : in STD_LOGIC;
    s_axil_awready : out STD_LOGIC;
    s_axil_wdata : in STD_LOGIC_VECTOR ( 31 downto 0 );
    s_axil_wstrb : in STD_LOGIC_VECTOR ( 3 downto 0 );
    s_axil_wvalid : in STD_LOGIC;
    s_axil_wready : out STD_LOGIC;
    s_axil_bvalid : out STD_LOGIC;
    s_axil_bresp : out STD_LOGIC_VECTOR ( 1 downto 0 );
    s_axil_bready : in STD_LOGIC;
    s_axil_araddr : in STD_LOGIC_VECTOR ( 31 downto 0 );
    s_axil_arprot : in STD_LOGIC_VECTOR ( 2 downto 0 );
    s_axil_arvalid : in STD_LOGIC;
    s_axil_arready : out STD_LOGIC;
    s_axil_rdata : out STD_LOGIC_VECTOR ( 31 downto 0 );
    s_axil_rresp : out STD_LOGIC_VECTOR ( 1 downto 0 );
    s_axil_rvalid : out STD_LOGIC;
    s_axil_rready : in STD_LOGIC;
    interrupt_out : out STD_LOGIC;
    s_axib_awid : in STD_LOGIC_VECTOR ( 3 downto 0 );
    s_axib_awaddr : in STD_LOGIC_VECTOR ( 31 downto 0 );
    s_axib_awregion : in STD_LOGIC_VECTOR ( 3 downto 0 );
    s_axib_awlen : in STD_LOGIC_VECTOR ( 7 downto 0 );
    s_axib_awsize : in STD_LOGIC_VECTOR ( 2 downto 0 );
    s_axib_awburst : in STD_LOGIC_VECTOR ( 1 downto 0 );
    s_axib_awvalid : in STD_LOGIC;
    s_axib_wdata : in STD_LOGIC_VECTOR ( 511 downto 0 );
    s_axib_wstrb : in STD_LOGIC_VECTOR ( 63 downto 0 );
    s_axib_wlast : in STD_LOGIC;
    s_axib_wvalid : in STD_LOGIC;
    s_axib_bready : in STD_LOGIC;
    s_axib_arid : in STD_LOGIC_VECTOR ( 3 downto 0 );
    s_axib_araddr : in STD_LOGIC_VECTOR ( 31 downto 0 );
    s_axib_arregion : in STD_LOGIC_VECTOR ( 3 downto 0 );
    s_axib_arlen : in STD_LOGIC_VECTOR ( 7 downto 0 );
    s_axib_arsize : in STD_LOGIC_VECTOR ( 2 downto 0 );
    s_axib_arburst : in STD_LOGIC_VECTOR ( 1 downto 0 );
    s_axib_arvalid : in STD_LOGIC;
    s_axib_rready : in STD_LOGIC;
    s_axib_awready : out STD_LOGIC;
    s_axib_wready : out STD_LOGIC;
    s_axib_bid : out STD_LOGIC_VECTOR ( 3 downto 0 );
    s_axib_bresp : out STD_LOGIC_VECTOR ( 1 downto 0 );
    s_axib_bvalid : out STD_LOGIC;
    s_axib_arready : out STD_LOGIC;
    s_axib_rid : out STD_LOGIC_VECTOR ( 3 downto 0 );
    s_axib_rdata : out STD_LOGIC_VECTOR ( 511 downto 0 );
    s_axib_rresp : out STD_LOGIC_VECTOR ( 1 downto 0 );
    s_axib_rlast : out STD_LOGIC;
    s_axib_rvalid : out STD_LOGIC
  );

end design_2_xdma_0_0;

architecture stub of design_2_xdma_0_0 is
attribute syn_black_box : boolean;
attribute black_box_pad_pin : string;
attribute syn_black_box of stub : architecture is true;
attribute black_box_pad_pin of stub : architecture is "sys_clk,sys_clk_gt,sys_rst_n,cfg_ltssm_state[5:0],user_lnk_up,pci_exp_txp[15:0],pci_exp_txn[15:0],pci_exp_rxp[15:0],pci_exp_rxn[15:0],axi_aclk,axi_aresetn,axi_ctl_aresetn,usr_irq_req[0:0],usr_irq_ack[0:0],msi_enable,msi_vector_width[2:0],s_axil_awaddr[31:0],s_axil_awprot[2:0],s_axil_awvalid,s_axil_awready,s_axil_wdata[31:0],s_axil_wstrb[3:0],s_axil_wvalid,s_axil_wready,s_axil_bvalid,s_axil_bresp[1:0],s_axil_bready,s_axil_araddr[31:0],s_axil_arprot[2:0],s_axil_arvalid,s_axil_arready,s_axil_rdata[31:0],s_axil_rresp[1:0],s_axil_rvalid,s_axil_rready,interrupt_out,s_axib_awid[3:0],s_axib_awaddr[31:0],s_axib_awregion[3:0],s_axib_awlen[7:0],s_axib_awsize[2:0],s_axib_awburst[1:0],s_axib_awvalid,s_axib_wdata[511:0],s_axib_wstrb[63:0],s_axib_wlast,s_axib_wvalid,s_axib_bready,s_axib_arid[3:0],s_axib_araddr[31:0],s_axib_arregion[3:0],s_axib_arlen[7:0],s_axib_arsize[2:0],s_axib_arburst[1:0],s_axib_arvalid,s_axib_rready,s_axib_awready,s_axib_wready,s_axib_bid[3:0],s_axib_bresp[1:0],s_axib_bvalid,s_axib_arready,s_axib_rid[3:0],s_axib_rdata[511:0],s_axib_rresp[1:0],s_axib_rlast,s_axib_rvalid";
attribute X_CORE_INFO : string;
attribute X_CORE_INFO of stub : architecture is "design_2_xdma_0_0_core_top,Vivado 2020.2";
begin
end;
