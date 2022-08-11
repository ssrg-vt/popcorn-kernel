//Copyright 1986-2020 Xilinx, Inc. All Rights Reserved.
//--------------------------------------------------------------------------------
//Tool Version: Vivado v.2020.2 (lin64) Build 3064766 Wed Nov 18 09:12:47 MST 2020
//Date        : Thu Mar  3 16:06:39 2022
//Host        : hemanthr-ssrg running 64-bit Ubuntu 18.04.6 LTS
//Command     : generate_target design_2_wrapper.bd
//Design      : design_2_wrapper
//Purpose     : IP block netlist
//--------------------------------------------------------------------------------
`timescale 1 ps / 1 ps

module design_2_wrapper
   (pcie_7x_mgt_rtl_0_rxn,
    pcie_7x_mgt_rtl_0_rxp,
    pcie_7x_mgt_rtl_0_txn,
    pcie_7x_mgt_rtl_0_txp,
    pcie_refclk1_clk_n,
    pcie_refclk1_clk_p,
    reset_rtl_0);
  input [15:0]pcie_7x_mgt_rtl_0_rxn;
  input [15:0]pcie_7x_mgt_rtl_0_rxp;
  output [15:0]pcie_7x_mgt_rtl_0_txn;
  output [15:0]pcie_7x_mgt_rtl_0_txp;
  input [0:0]pcie_refclk1_clk_n;
  input [0:0]pcie_refclk1_clk_p;
  input reset_rtl_0;

  wire [15:0]pcie_7x_mgt_rtl_0_rxn;
  wire [15:0]pcie_7x_mgt_rtl_0_rxp;
  wire [15:0]pcie_7x_mgt_rtl_0_txn;
  wire [15:0]pcie_7x_mgt_rtl_0_txp;
  wire [0:0]pcie_refclk1_clk_n;
  wire [0:0]pcie_refclk1_clk_p;
  wire reset_rtl_0;

  design_2 design_2_i
       (.pcie_7x_mgt_rtl_0_rxn(pcie_7x_mgt_rtl_0_rxn),
        .pcie_7x_mgt_rtl_0_rxp(pcie_7x_mgt_rtl_0_rxp),
        .pcie_7x_mgt_rtl_0_txn(pcie_7x_mgt_rtl_0_txn),
        .pcie_7x_mgt_rtl_0_txp(pcie_7x_mgt_rtl_0_txp),
        .pcie_refclk1_clk_n(pcie_refclk1_clk_n),
        .pcie_refclk1_clk_p(pcie_refclk1_clk_p),
        .reset_rtl_0(reset_rtl_0));
endmodule
