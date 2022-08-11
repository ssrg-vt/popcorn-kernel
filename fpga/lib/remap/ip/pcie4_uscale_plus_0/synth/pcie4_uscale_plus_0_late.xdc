##-----------------------------------------------------------------------------
##
## (c) Copyright 2012-2012 Xilinx, Inc. All rights reserved.
##
## This file contains confidential and proprietary information
## of Xilinx, Inc. and is protected under U.S. and
## international copyright and other intellectual property
## laws.
##
## DISCLAIMER
## This disclaimer is not a license and does not grant any
## rights to the materials distributed herewith. Except as
## otherwise provided in a valid license issued to you by
## Xilinx, and to the maximum extent permitted by applicable
## law: (1) THESE MATERIALS ARE MADE AVAILABLE "AS IS" AND
## WITH ALL FAULTS, AND XILINX HEREBY DISCLAIMS ALL WARRANTIES
## AND CONDITIONS, EXPRESS, IMPLIED, OR STATUTORY, INCLUDING
## BUT NOT LIMITED TO WARRANTIES OF MERCHANTABILITY, NON-
## INFRINGEMENT, OR FITNESS FOR ANY PARTICULAR PURPOSE; and
## (2) Xilinx shall not be liable (whether in contract or tort,
## including negligence, or under any other theory of
## liability) for any loss or damage of any kind or nature
## related to, arising under or in connection with these
## materials, including for any direct, or any indirect,
## special, incidental, or consequential loss or damage
## (including loss of data, profits, goodwill, or any type of
## loss or damage suffered as a result of any action brought
## by a third party) even if such damage or loss was
## reasonably foreseeable or Xilinx had been advised of the
## possibility of the same.
##
## CRITICAL APPLICATIONS
## Xilinx products are not designed or intended to be fail-
## safe, or for use in any application requiring fail-safe
## performance, such as life-support or safety devices or
## systems, Class III medical devices, nuclear facilities,
## applications related to the deployment of airbags, or any
## other applications that could lead to death, personal
## injury, or severe property or environmental damage
## (individually and collectively, "Critical
## Applications"). Customer assumes the sole risk and
## liability of any use of Xilinx products in Critical
## Applications, subject only to applicable laws and
## regulations governing limitations on product liability.
##
## THIS COPYRIGHT NOTICE AND DISCLAIMER MUST BE RETAINED AS
## PART OF THIS FILE AT ALL TIMES.
##
##-----------------------------------------------------------------------------
##
## Project    : UltraScale+ FPGA PCI Express v4.0 Integrated Block
## File       : pcie4_uscale_plus_0_late.xdc
## Version    : 1.3 
##-----------------------------------------------------------------------------
#
# This constraints file contains ASYNC clock grouping and processed late after OOC and IP Level XDC files. 
#
#
###############################################################################
# ASYNC CLOCK GROUPINGS
###############################################################################
# sys_clk vs TXOUTCLK
set_clock_groups -asynchronous -group [get_clocks -of_objects [get_ports sys_clk]] -group [get_clocks -of_objects [get_pins -hierarchical -filter {NAME =~ *gen_channel_container[3].*gen_gthe4_channel_inst[3].GTHE4_CHANNEL_PRIM_INST/TXOUTCLK}]]
set_clock_groups -asynchronous -group [get_clocks -of_objects [get_pins -hierarchical -filter {NAME =~ *gen_channel_container[3].*gen_gthe4_channel_inst[3].GTHE4_CHANNEL_PRIM_INST/TXOUTCLK}]] -group [get_clocks -of_objects [get_ports sys_clk]]
#
# sys_clk vs intclk
set_clock_groups -asynchronous -group [get_clocks -of_objects [get_pins pcie4_uscale_plus_0_gt_top_i/diablo_gt.diablo_gt_phy_wrapper/phy_clk_i/bufg_gt_intclk/O]] -group [get_clocks -of_objects [get_ports sys_clk]]
set_clock_groups -asynchronous -group [get_clocks -of_objects [get_ports sys_clk]] -group [get_clocks -of_objects [get_pins pcie4_uscale_plus_0_gt_top_i/diablo_gt.diablo_gt_phy_wrapper/phy_clk_i/bufg_gt_intclk/O]]
#
# intclk vs pclk
set_clock_groups -asynchronous -group [get_clocks -of_objects [get_pins pcie4_uscale_plus_0_gt_top_i/diablo_gt.diablo_gt_phy_wrapper/phy_clk_i/bufg_gt_intclk/O]] -group [get_clocks -of_objects [get_pins pcie4_uscale_plus_0_gt_top_i/diablo_gt.diablo_gt_phy_wrapper/phy_clk_i/bufg_gt_pclk/O]]
set_clock_groups -asynchronous -group [get_clocks -of_objects [get_pins pcie4_uscale_plus_0_gt_top_i/diablo_gt.diablo_gt_phy_wrapper/phy_clk_i/bufg_gt_pclk/O]] -group [get_clocks -of_objects [get_pins pcie4_uscale_plus_0_gt_top_i/diablo_gt.diablo_gt_phy_wrapper/phy_clk_i/bufg_gt_intclk/O]]
#
# sys_clk vs pclk
set_clock_groups -asynchronous -group [get_clocks -of_objects [get_ports sys_clk]] -group [get_clocks -of_objects [get_pins pcie4_uscale_plus_0_gt_top_i/diablo_gt.diablo_gt_phy_wrapper/phy_clk_i/bufg_gt_pclk/O]]
set_clock_groups -asynchronous -group [get_clocks -of_objects [get_pins pcie4_uscale_plus_0_gt_top_i/diablo_gt.diablo_gt_phy_wrapper/phy_clk_i/bufg_gt_pclk/O]] -group [get_clocks -of_objects [get_ports sys_clk]]
#
#create_waiver -type METHODOLOGY -id {TIMING-1} -desc "SAFELYcanIGNORE" 
#create_waiver -type METHODOLOGY -id {TIMING-3} -desc "SAFELYcanIGNORE" 
#create_waiver -type METHODOLOGY -id {TIMING-9} -desc "SAFELYcanIGNORE" 
#
create_waiver -internal -quiet -user pcie4_uscale_plus -tags 1015842 -type METHODOLOGY -id TIMING-3 -description "added waiver as TXOUTCLK period is intentionally overriding as DRP ports used to change the configuration/speed dynamically in runtime"  -scope \
  -objects [get_pins -hierarchical -filter {NAME =~ *gen_channel_container[3].*gen_gthe4_channel_inst[3].GT*E4_CHANNEL_PRIM_INST/TXOUTCLK}]
#
create_waiver -internal -quiet -user pcie4_uscale_plus -tags 1015842 -type METHODOLOGY -id TIMING-1 -description "added waiver as TXOUTCLK period is intentionally overriding as DRP ports used to change the configuration/speed dynamically in runtime"  -scope \
  -objects [get_pins -hierarchical -filter {NAME =~ *gen_channel_container[3].*gen_gthe4_channel_inst[3].GT*E4_CHANNEL_PRIM_INST/TXOUTCLK}]
#
create_waiver -internal -quiet -user pcie4_uscale_plus -tags 1015842 -type METHODOLOGY -id TIMING-3 -description "added waiver for int clock BUFG_GT usecase and this clock will be used for small portion of the logic before perst is deasserted/removed"  -scope \
  -objects [get_pins -filter {REF_PIN_NAME=~*O} -of_objects [get_cells -hierarchical -filter {NAME =~ *phy_clk_i/bufg_gt_intclk}]]
