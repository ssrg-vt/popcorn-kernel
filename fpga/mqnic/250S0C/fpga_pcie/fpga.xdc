# XDC constraints for the Bittware 250SoC
# part: xczu19eg-ffvd1760-2-e

# General configuration
set_property BITSTREAM.GENERAL.COMPRESS true [current_design]

# System clocks
# 200 MHz
set_property -dict {LOC J19 IOSTANDARD DIFF_SSTL12} [get_ports clk_125mhz_p]
set_property -dict {LOC J18 IOSTANDARD DIFF_SSTL12} [get_ports clk_125mhz_n]
create_clock -period 8.000 -name clk_125mhz [get_ports clk_125mhz_p]
#set_input_jitter [get_clocks -of_objects [get_ports clk_125mhz_p]] 0.1

# LEDs
set_property -dict {LOC B12 IOSTANDARD LVCMOS33} [get_ports {led[0]}]
set_property -dict {LOC B13 IOSTANDARD LVCMOS33} [get_ports {led[1]}]
set_property -dict {LOC B10 IOSTANDARD LVCMOS33} [get_ports {led[2]}]
set_property -dict {LOC B11 IOSTANDARD LVCMOS33} [get_ports {led[3]}]

set_false_path -to [get_ports {led[*]}]
set_output_delay 0.000 [get_ports {led[*]}]

# Reset button
#set_property -dict {LOC G13  IOSTANDARD LVCMOS12} [get_ports reset]

#set_false_path -from [get_ports {reset}]
#set_input_delay 0 [get_ports {reset}]

# UART
#set_property -dict {LOC AL17 IOSTANDARD LVCMOS12 SLEW SLOW DRIVE 8} [get_ports uart_txd]
#set_property -dict {LOC AH17 IOSTANDARD LVCMOS12} [get_ports uart_rxd]
#set_property -dict {LOC AM15 IOSTANDARD LVCMOS12} [get_ports uart_rts]
#set_property -dict {LOC AP17 IOSTANDARD LVCMOS12 SLEW SLOW DRIVE 8} [get_ports uart_cts]

#set_false_path -to [get_ports {uart_txd uart_cts}]
#set_output_delay 0 [get_ports {uart_txd uart_cts}]
#set_false_path -from [get_ports {uart_rxd uart_rts}]
#set_input_delay 0 [get_ports {uart_rxd uart_rts}]

# I2C interfaces
#set_property -dict {LOC AE19 IOSTANDARD LVCMOS12} [get_ports i2c0_scl]
#set_property -dict {LOC AH23 IOSTANDARD LVCMOS12} [get_ports i2c0_sda]
#set_property -dict {LOC AH19 IOSTANDARD LVCMOS12} [get_ports i2c1_scl]
#set_property -dict {LOC AL21 IOSTANDARD LVCMOS12} [get_ports i2c1_sda]

#set_false_path -to [get_ports {i2c1_sda i2c1_scl}]
#set_output_delay 0 [get_ports {i2c1_sda i2c1_scl}]
#set_false_path -from [get_ports {i2c1_sda i2c1_scl}]
#set_input_delay 0 [get_ports {i2c1_sda i2c1_scl}]

set_property -dict {LOC H14 IOSTANDARD LVCMOS33} [get_ports i2c_scl]
set_property -dict {LOC H15 IOSTANDARD LVCMOS33} [get_ports i2c_sda]

set_false_path -to [get_ports {i2c_sda i2c_scl}]
set_output_delay 0.000 [get_ports {i2c_sda i2c_scl}]
set_false_path -from [get_ports {i2c_sda i2c_scl}]
set_input_delay 0.000 [get_ports {i2c_sda i2c_scl}]

# QSFP28 Interface
set_property -dict {LOC H41} [get_ports qsfp_0_rx_0_p]
#set_property -dict {LOC H42  } [get_ports qsfp_0_rx_0_n] ;# MGTYRXN0_133 GTYE4_CHANNEL_X0Y16 / GTYE4_COMMON_X0Y4
set_property -dict {LOC H36} [get_ports qsfp_0_tx_0_p]
#set_property -dict {LOC H37  } [get_ports qsfp_0_tx_0_n] ;# MGTYTXN0_133 GTYE4_CHANNEL_X0Y16 / GTYE4_COMMON_X0Y4
set_property -dict {LOC G39} [get_ports qsfp_0_rx_1_p]
#set_property -dict {LOC G40  } [get_ports qsfp_0_rx_1_n] ;# MGTYRXN1_133 GTYE4_CHANNEL_X0Y17 / GTYE4_COMMON_X0Y4
set_property -dict {LOC G34} [get_ports qsfp_0_tx_1_p]
#set_property -dict {LOC G35  } [get_ports qsfp_0_tx_1_n] ;# MGTYTXN1_133 GTYE4_CHANNEL_X0Y17 / GTYE4_COMMON_X0Y4
set_property -dict {LOC F41} [get_ports qsfp_0_rx_2_p]
#set_property -dict {LOC F42  } [get_ports qsfp_0_rx_2_n] ;# MGTYRXN2_133 GTYE4_CHANNEL_X0Y18 / GTYE4_COMMON_X0Y4
set_property -dict {LOC F36} [get_ports qsfp_0_tx_2_p]
#set_property -dict {LOC F37  } [get_ports qsfp_0_tx_2_n] ;# MGTYTXN2_133 GTYE4_CHANNEL_X0Y18 / GTYE4_COMMON_X0Y4
set_property -dict {LOC E39} [get_ports qsfp_0_rx_3_p]
#set_property -dict {LOC E40  } [get_ports qsfp_0_rx_3_n] ;# MGTYRXN3_133 GTYE4_CHANNEL_X0Y19 / GTYE4_COMMON_X0Y4
set_property -dict {LOC E34} [get_ports qsfp_0_tx_3_p]
#set_property -dict {LOC E35  } [get_ports qsfp_0_tx_3_n] ;# MGTYTXN3_133 GTYE4_CHANNEL_X0Y19 / GTYE4_COMMON_X0Y4
set_property -dict {LOC H32} [get_ports qsfp_0_mgt_refclk_p]
set_property -dict {LOC H33} [get_ports qsfp_0_mgt_refclk_n]
set_property PACKAGE_PIN J15 [get_ports qsfp_0_mod_prsnt_n]
set_property IOSTANDARD LVCMOS33 [get_ports qsfp_0_mod_prsnt_n]
set_property PULLUP true [get_ports qsfp_0_mod_prsnt_n]
set_property PACKAGE_PIN L13 [get_ports qsfp_0_reset_n]
set_property IOSTANDARD LVCMOS33 [get_ports qsfp_0_reset_n]
set_property PULLUP true [get_ports qsfp_0_reset_n]
set_property PACKAGE_PIN L12 [get_ports qsfp_0_lp_mode]
set_property IOSTANDARD LVCMOS33 [get_ports qsfp_0_lp_mode]
set_property PULLDOWN true [get_ports qsfp_0_lp_mode]
set_property PACKAGE_PIN K15 [get_ports qsfp_0_intr_n]
set_property IOSTANDARD LVCMOS33 [get_ports qsfp_0_intr_n]
set_property PULLUP true [get_ports qsfp_0_intr_n]
#set_property -dict {LOC H15   IOSTANDARD LVCMOS33} [get_ports qsfp_0_i2c_scl]
#set_property -dict {LOC H14   IOSTANDARD LVCMOS33} [get_ports qsfp_0_i2c_sda]

# 322.25 MHz MGT reference clock
create_clock -period 3.103 -name qsfp_0_mgt_refclk [get_ports qsfp_0_mgt_refclk_p]

set_false_path -to [get_ports {qsfp_0_reset_n qsfp_0_lp_mode}]
set_output_delay 0.000 [get_ports {qsfp_0_reset_n qsfp_0_lp_mode}]
set_false_path -from [get_ports {qsfp_0_mod_prsnt_n qsfp_0_intr_n}]
set_input_delay 0.000 [get_ports {qsfp_0_mod_prsnt_n qsfp_0_intr_n}]

#set_false_path -to [get_ports {qsfp_0_i2c_scl qsfp_0_i2c_sda}]
#set_output_delay 0 [get_ports {qsfp_0_i2c_scl qsfp_0_i2c_sda}]
#set_false_path -from [get_ports {qsfp_0_i2c_scl qsfp_0_i2c_sda}]
#set_input_delay 0 [get_ports {qsfp_0_i2c_scl qsfp_0_i2c_sda}]

set_property -dict {LOC D41} [get_ports qsfp_1_rx_0_p]
set_property -dict {LOC D42} [get_ports qsfp_1_rx_0_n]
set_property -dict {LOC D36} [get_ports qsfp_1_tx_0_p]
set_property -dict {LOC D37} [get_ports qsfp_1_tx_0_n]
set_property -dict {LOC C39} [get_ports qsfp_1_rx_1_p]
set_property -dict {LOC C40} [get_ports qsfp_1_rx_1_n]
set_property -dict {LOC C34} [get_ports qsfp_1_tx_1_p]
set_property -dict {LOC C35} [get_ports qsfp_1_tx_1_n]
set_property -dict {LOC B41} [get_ports qsfp_1_rx_2_p]
set_property -dict {LOC B42} [get_ports qsfp_1_rx_2_n]
set_property -dict {LOC B36} [get_ports qsfp_1_tx_2_p]
set_property -dict {LOC B37} [get_ports qsfp_1_tx_2_n]
set_property -dict {LOC A39} [get_ports qsfp_1_rx_3_p]
set_property -dict {LOC A40} [get_ports qsfp_1_rx_3_n]
set_property -dict {LOC A34} [get_ports qsfp_1_tx_3_p]
set_property -dict {LOC A35} [get_ports qsfp_1_tx_3_n]
set_property -dict {LOC D32} [get_ports qsfp_1_mgt_refclk_p]
set_property -dict {LOC D33} [get_ports qsfp_1_mgt_refclk_n]
set_property PACKAGE_PIN J13 [get_ports qsfp_1_mod_prsnt_n]
set_property IOSTANDARD LVCMOS33 [get_ports qsfp_1_mod_prsnt_n]
set_property PULLUP true [get_ports qsfp_1_mod_prsnt_n]
set_property PACKAGE_PIN K13 [get_ports qsfp_1_reset_n]
set_property IOSTANDARD LVCMOS33 [get_ports qsfp_1_reset_n]
set_property PULLUP true [get_ports qsfp_1_reset_n]
set_property PACKAGE_PIN K12 [get_ports qsfp_1_lp_mode]
set_property IOSTANDARD LVCMOS33 [get_ports qsfp_1_lp_mode]
set_property PULLDOWN true [get_ports qsfp_1_lp_mode]
set_property PACKAGE_PIN J14 [get_ports qsfp_1_intr_n]
set_property IOSTANDARD LVCMOS33 [get_ports qsfp_1_intr_n]
set_property PULLUP true [get_ports qsfp_1_intr_n]
#set_property -dict {LOC H15   IOSTANDARD LVCMOS33} [get_ports qsfp_1_i2c_scl]
#set_property -dict {LOC H14   IOSTANDARD LVCMOS33} [get_ports qsfp_1_i2c_sda]

# 322.23 MHz MGT reference clock
create_clock -period 3.103 -name qsfp_1_mgt_refclk [get_ports qsfp_1_mgt_refclk_p]

set_false_path -to [get_ports {qsfp_1_reset_n qsfp_1_lp_mode}]
set_output_delay 0.000 [get_ports {qsfp_1_reset_n qsfp_1_lp_mode}]
set_false_path -from [get_ports {qsfp_1_mod_prsnt_n qsfp_1_intr_n}]
set_input_delay 0.000 [get_ports {qsfp_1_mod_prsnt_n qsfp_1_intr_n}]

#set_false_path -to [get_ports {qsfp_1_i2c_scl qsfp_1_i2c_sda}]
#set_output_delay 0 [get_ports {qsfp_1_i2c_scl qsfp_1_i2c_sda}]
#set_false_path -from [get_ports {qsfp_1_i2c_scl qsfp_1_i2c_sda}]
#set_input_delay 0 [get_ports {qsfp_1_i2c_scl qsfp_1_i2c_sda}]

#set_false_path -to [get_ports {sfp0_tx_disable_b sfp1_tx_disable_b}]
#set_output_delay 0 [get_ports {sfp0_tx_disable_b sfp1_tx_disable_b}]

# PCIe Interface
#set_property -dict {LOC AE2 } [get_ports {pcie_rx_p[0]}] ;# MGTHRXP3_224 GTHE4_CHANNEL_X0Y7 / GTHE4_COMMON_X0Y1
#set_property -dict {LOC AE1 } [get_ports {pcie_rx_n[0]}] ;# MGTHRXN3_224 GTHE4_CHANNEL_X0Y7 / GTHE4_COMMON_X0Y1
#set_property -dict {LOC AD4 } [get_ports {pcie_tx_p[0]}] ;# MGTHTXP3_224 GTHE4_CHANNEL_X0Y7 / GTHE4_COMMON_X0Y1
#set_property -dict {LOC AD3 } [get_ports {pcie_tx_n[0]}] ;# MGTHTXN3_224 GTHE4_CHANNEL_X0Y7 / GTHE4_COMMON_X0Y1
#set_property -dict {LOC AF4 } [get_ports {pcie_rx_p[1]}] ;# MGTHRXP2_224 GTHE4_CHANNEL_X0Y6 / GTHE4_COMMON_X0Y1
#set_property -dict {LOC AF3 } [get_ports {pcie_rx_n[1]}] ;# MGTHRXN2_224 GTHE4_CHANNEL_X0Y6 / GTHE4_COMMON_X0Y1
#set_property -dict {LOC AE6 } [get_ports {pcie_tx_p[1]}] ;# MGTHTXP2_224 GTHE4_CHANNEL_X0Y6 / GTHE4_COMMON_X0Y1
#set_property -dict {LOC AE5 } [get_ports {pcie_tx_n[1]}] ;# MGTHTXN2_224 GTHE4_CHANNEL_X0Y6 / GTHE4_COMMON_X0Y1
#set_property -dict {LOC AG2 } [get_ports {pcie_rx_p[2]}] ;# MGTHRXP1_224 GTHE4_CHANNEL_X0Y5 / GTHE4_COMMON_X0Y1
#set_property -dict {LOC AG1 } [get_ports {pcie_rx_n[2]}] ;# MGTHRXN1_224 GTHE4_CHANNEL_X0Y5 / GTHE4_COMMON_X0Y1
#set_property -dict {LOC AG6 } [get_ports {pcie_tx_p[2]}] ;# MGTHTXP1_224 GTHE4_CHANNEL_X0Y5 / GTHE4_COMMON_X0Y1
#set_property -dict {LOC AG5 } [get_ports {pcie_tx_n[2]}] ;# MGTHTXN1_224 GTHE4_CHANNEL_X0Y5 / GTHE4_COMMON_X0Y1
#set_property -dict {LOC AJ2 } [get_ports {pcie_rx_p[3]}] ;# MGTHRXP0_224 GTHE4_CHANNEL_X0Y4 / GTHE4_COMMON_X0Y1
#set_property -dict {LOC AJ1 } [get_ports {pcie_rx_n[3]}] ;# MGTHRXN0_224 GTHE4_CHANNEL_X0Y4 / GTHE4_COMMON_X0Y1
#set_property -dict {LOC AH4 } [get_ports {pcie_tx_p[3]}] ;# MGTHTXP0_224 GTHE4_CHANNEL_X0Y4 / GTHE4_COMMON_X0Y1
#set_property -dict {LOC AH3 } [get_ports {pcie_tx_n[3]}] ;# MGTHTXN0_224 GTHE4_CHANNEL_X0Y4 / GTHE4_COMMON_X0Y1i

#set_property -dict {LOC BB6 } [get_ports {pcie_rx_p[0]}]  ;# MGTHRXP3_224 GTHE4_CHANNEL_X0Y15 / GTHE4_COMMON_X0Y3
#set_property -dict {LOC BB5 } [get_ports {pcie_rx_n[0]}]  ;# MGTHRXN3_224 GTHE4_CHANNEL_X0Y15 / GTHE4_COMMON_X0Y3
#set_property -dict {LOC BA8 } [get_ports {pcie_tx_p[0]}]  ;# MGTHTXP3_224 GTHE4_CHANNEL_X0Y15 / GTHE4_COMMON_X0Y3
#set_property -dict {LOC BA7 } [get_ports {pcie_tx_n[0]}]  ;# MGTHTXN3_224 GTHE4_CHANNEL_X0Y15 / GTHE4_COMMON_X0Y3
#set_property -dict {LOC BA4 } [get_ports {pcie_rx_p[1]}]  ;# MGTHRXP2_224 GTHE4_CHANNEL_X0Y14 / GTHE4_COMMON_X0Y3
#set_property -dict {LOC BA3 } [get_ports {pcie_rx_n[1]}]  ;# MGTHRXN2_224 GTHE4_CHANNEL_X0Y14 / GTHE4_COMMON_X0Y3
#set_property -dict {LOC AW8 } [get_ports {pcie_tx_p[1]}]  ;# MGTHTXP2_224 GTHE4_CHANNEL_X0Y14 / GTHE4_COMMON_X0Y3
#set_property -dict {LOC AW7 } [get_ports {pcie_tx_n[1]}]  ;# MGTHTXN2_224 GTHE4_CHANNEL_X0Y14 / GTHE4_COMMON_X0Y3
#set_property -dict {LOC AG2 } [get_ports {pcie_rx_p[2]}] ;# MGTHRXP1_224 GTHE4_CHANNEL_X0Y5 / GTHE4_COMMON_X0Y1
#set_property -dict {LOC AY5 } [get_ports {pcie_rx_n[2]}]  ;# MGTHRXN1_224 GTHE4_CHANNEL_X0Y13 / GTHE4_COMMON_X0Y3
#set_property -dict {LOC AV6 } [get_ports {pcie_tx_p[2]}]  ;# MGTHTXP1_224 GTHE4_CHANNEL_X0Y13 / GTHE4_COMMON_X0Y3
#set_property -dict {LOC AV5 } [get_ports {pcie_tx_n[2]}]  ;# MGTHTXN1_224 GTHE4_CHANNEL_X0Y13 / GTHE4_COMMON_X0Y3
#set_property -dict {LOC AY2 } [get_ports {pcie_rx_p[3]}]  ;# MGTHRXP0_224 GTHE4_CHANNEL_X0Y12 / GTHE4_COMMON_X0Y3
#set_property -dict {LOC AY1 } [get_ports {pcie_rx_n[3]}]  ;# MGTHRXN0_224 GTHE4_CHANNEL_X0Y12 / GTHE4_COMMON_X0Y3
#set_property -dict {LOC AU8 } [get_ports {pcie_tx_p[3]}]  ;# MGTHTXP0_224 GTHE4_CHANNEL_X0Y12 / GTHE4_COMMON_X0Y3
#set_property -dict {LOC AU7 } [get_ports {pcie_tx_n[3]}]  ;# MGTHTXN0_224 GTHE4_CHANNEL_X0Y12 / GTHE4_COMMON_X0Y3
#set_property -dict {LOC AW4 } [get_ports {pcie_rx_p[4]}]  ;# MGTHRXP3_225 GTHE4_CHANNEL_X0Y11 / GTHE4_COMMON_X0Y2
#set_property -dict {LOC AW3 } [get_ports {pcie_rx_n[4]}]  ;# MGTHRXN3_225 GTHE4_CHANNEL_X0Y11 / GTHE4_COMMON_X0Y2
#set_property -dict {LOC AT10} [get_ports {pcie_tx_p[4]}]  ;# MGTHTXP3_225 GTHE4_CHANNEL_X0Y11 / GTHE4_COMMON_X0Y2
#set_property -dict {LOC AT9 } [get_ports {pcie_tx_n[4]}]  ;# MGTHTXN3_225 GTHE4_CHANNEL_X0Y11 / GTHE4_COMMON_X0Y2
#set_property -dict {LOC AV2 } [get_ports {pcie_rx_p[5]}]  ;# MGTHRXP2_225 GTHE4_CHANNEL_X0Y10 / GTHE4_COMMON_X0Y2
#set_property -dict {LOC AV1 } [get_ports {pcie_rx_n[5]}]  ;# MGTHRXN2_225 GTHE4_CHANNEL_X0Y10 / GTHE4_COMMON_X0Y2
#set_property -dict {LOC AT6 } [get_ports {pcie_tx_p[5]}]  ;# MGTHTXP2_225 GTHE4_CHANNEL_X0Y10 / GTHE4_COMMON_X0Y2
#set_property -dict {LOC AT5 } [get_ports {pcie_tx_n[5]}]  ;# MGTHTXN2_225 GTHE4_CHANNEL_X0Y10 / GTHE4_COMMON_X0Y2
#set_property -dict {LOC AU4 } [get_ports {pcie_rx_p[6]}]  ;# MGTHRXP1_225 GTHE4_CHANNEL_X0Y9 / GTHE4_COMMON_X0Y2
#set_property -dict {LOC AU3 } [get_ports {pcie_rx_n[6]}]  ;# MGTHRXN1_225 GTHE4_CHANNEL_X0Y9 / GTHE4_COMMON_X0Y2
#set_property -dict {LOC AR8 } [get_ports {pcie_tx_p[6]}]  ;# MGTHTXP1_225 GTHE4_CHANNEL_X0Y9 / GTHE4_COMMON_X0Y2
#set_property -dict {LOC AR7 } [get_ports {pcie_tx_n[6]}]  ;# MGTHTXN1_225 GTHE4_CHANNEL_X0Y9 / GTHE4_COMMON_X0Y2
#set_property -dict {LOC AT2 } [get_ports {pcie_rx_p[7]}]  ;# MGTHRXP0_225 GTHE4_CHANNEL_X0Y8 / GTHE4_COMMON_X0Y2
#set_property -dict {LOC AT1 } [get_ports {pcie_rx_n[7]}]  ;# MGTHRXN0_225 GTHE4_CHANNEL_X0Y8 / GTHE4_COMMON_X0Y2
#set_property -dict {LOC AP10} [get_ports {pcie_tx_p[7]}]  ;# MGTHTXP0_225 GTHE4_CHANNEL_X0Y8 / GTHE4_COMMON_X0Y2
#set_property -dict {LOC AP9 } [get_ports {pcie_tx_n[7]}]  ;# MGTHTXN0_225 GTHE4_CHANNEL_X0Y8 / GTHE4_COMMON_X0Y2

#set_property -dict {LOC AR4 } [get_ports {pcie_rx_p[8]}]  ;# MGTHRXP3_226 GTHE4_CHANNEL_X0Y7 / GTHE4_COMMON_X0Y1
#set_property -dict {LOC AR3 } [get_ports {pcie_rx_n[8]}]  ;# MGTHRXN3_226 GTHE4_CHANNEL_X0Y7 / GTHE4_COMMON_X0Y1
#set_property -dict {LOC AP6 } [get_ports {pcie_tx_p[8]}]  ;# MGTHTXP3_226 GTHE4_CHANNEL_X0Y7 / GTHE4_COMMON_X0Y1
#set_property -dict {LOC AP5 } [get_ports {pcie_tx_n[8]}]  ;# MGTHTXN3_226 GTHE4_CHANNEL_X0Y7 / GTHE4_COMMON_X0Y1
#set_property -dict {LOC AP2 } [get_ports {pcie_rx_p[9]}]  ;# MGTHRXP2_226 GTHE4_CHANNEL_X0Y6 / GTHE4_COMMON_X0Y1
#set_property -dict {LOC AP1 } [get_ports {pcie_rx_n[9]}]  ;# MGTHRXN2_226 GTHE4_CHANNEL_X0Y6 / GTHE4_COMMON_X0Y1
#set_property -dict {LOC AN8 } [get_ports {pcie_tx_p[9]}]  ;# MGTHTXP2_226 GTHE4_CHANNEL_X0Y6 / GTHE4_COMMON_X0Y1
#set_property -dict {LOC AN7 } [get_ports {pcie_tx_n[9]}]  ;# MGTHTXN2_226 GTHE4_CHANNEL_X0Y6 / GTHE4_COMMON_X0Y1
#set_property -dict {LOC AN4 } [get_ports {pcie_rx_p[10]}] ;# MGTHRXP1_226 GTHE4_CHANNEL_X0Y5 / GTHE4_COMMON_X0Y1
#set_property -dict {LOC AN3 } [get_ports {pcie_rx_n[10]}] ;# MGTHRXN1_226 GTHE4_CHANNEL_X0Y5 / GTHE4_COMMON_X0Y1
#set_property -dict {LOC AM6 } [get_ports {pcie_tx_p[10]}] ;# MGTHTXP1_226 GTHE4_CHANNEL_X0Y5 / GTHE4_COMMON_X0Y1
#set_property -dict {LOC AM5 } [get_ports {pcie_tx_n[10]}] ;# MGTHTXN1_226 GTHE4_CHANNEL_X0Y5 / GTHE4_COMMON_X0Y1
#set_property -dict {LOC AM2 } [get_ports {pcie_rx_p[11]}] ;# MGTHRXP0_226 GTHE4_CHANNEL_X0Y4 / GTHE4_COMMON_X0Y1
#set_property -dict {LOC AM1 } [get_ports {pcie_rx_n[11]}] ;# MGTHRXN0_226 GTHE4_CHANNEL_X0Y4 / GTHE4_COMMON_X0Y1
#set_property -dict {LOC AL8 } [get_ports {pcie_tx_p[11]}] ;# MGTHTXP0_226 GTHE4_CHANNEL_X0Y4 / GTHE4_COMMON_X0Y1
#set_property -dict {LOC AL7 } [get_ports {pcie_tx_n[11]}] ;# MGTHTXN0_226 GTHE4_CHANNEL_X0Y4 / GTHE4_COMMON_X0Y1
#set_property -dict {LOC AL4 } [get_ports {pcie_rx_p[12]}] ;# MGTHRXP3_227 GTHE4_CHANNEL_X0Y3 / GTHE4_COMMON_X0Y0
#set_property -dict {LOC AL3 } [get_ports {pcie_rx_n[12]}] ;# MGTHRXN3_227 GTHE4_CHANNEL_X0Y3 / GTHE4_COMMON_X0Y0
#set_property -dict {LOC AK6 } [get_ports {pcie_tx_p[12]}] ;# MGTHTXP3_227 GTHE4_CHANNEL_X0Y3 / GTHE4_COMMON_X0Y0
#set_property -dict {LOC AK5 } [get_ports {pcie_tx_n[12]}] ;# MGTHTXN3_227 GTHE4_CHANNEL_X0Y3 / GTHE4_COMMON_X0Y0
#set_property -dict {LOC AK2 } [get_ports {pcie_rx_p[13]}] ;# MGTHRXP2_227 GTHE4_CHANNEL_X0Y2 / GTHE4_COMMON_X0Y0
#set_property -dict {LOC AK1 } [get_ports {pcie_rx_n[13]}] ;# MGTHRXN2_227 GTHE4_CHANNEL_X0Y2 / GTHE4_COMMON_X0Y0
#set_property -dict {LOC AJ8 } [get_ports {pcie_tx_p[13]}] ;# MGTHTXP2_227 GTHE4_CHANNEL_X0Y2 / GTHE4_COMMON_X0Y0
#set_property -dict {LOC AJ7 } [get_ports {pcie_tx_n[13]}] ;# MGTHTXN2_227 GTHE4_CHANNEL_X0Y2 / GTHE4_COMMON_X0Y0
#set_property -dict {LOC AJ4 } [get_ports {pcie_rx_p[14]}] ;# MGTHRXP1_227 GTHE4_CHANNEL_X0Y1 / GTHE4_COMMON_X0Y0
#set_property -dict {LOC AJ3 } [get_ports {pcie_rx_n[14]}] ;# MGTHRXN1_227 GTHE4_CHANNEL_X0Y1 / GTHE4_COMMON_X0Y0
#set_property -dict {LOC AH6 } [get_ports {pcie_tx_p[14]}] ;# MGTHTXP1_227 GTHE4_CHANNEL_X0Y1 / GTHE4_COMMON_X0Y0
#set_property -dict {LOC AH5 } [get_ports {pcie_tx_n[14]}] ;# MGTHTXN1_227 GTHE4_CHANNEL_X0Y1 / GTHE4_COMMON_X0Y0
#set_property -dict {LOC AH2 } [get_ports {pcie_rx_p[15]}] ;# MGTHRXP0_227 GTHE4_CHANNEL_X0Y0 / GTHE4_COMMON_X0Y0
#set_property -dict {LOC AH1 } [get_ports {pcie_rx_n[15]}] ;# MGTHRXN0_227 GTHE4_CHANNEL_X0Y0 / GTHE4_COMMON_X0Y0
#set_property -dict {LOC AG8 } [get_ports {pcie_tx_p[15]}] ;# MGTHTXP0_227 GTHE4_CHANNEL_X0Y0 / GTHE4_COMMON_X0Y0
#set_property -dict {LOC AG7 } [get_ports {pcie_tx_n[15]}] ;# MGTHTXN0_227 GTHE4_CHANNEL_X0Y0 / GTHE4_COMMON_X0Y0

set_property -dict {LOC AH10} [get_ports pcie_refclk_p]
set_property -dict {LOC AH9} [get_ports pcie_refclk_n]
set_property PACKAGE_PIN AM16 [get_ports pcie_rst_n]
set_property IOSTANDARD LVCMOS18 [get_ports pcie_rst_n]
set_property PULLUP true [get_ports pcie_rst_n]
#set_property -dict {LOC AM10} [get_ports pcie_refclk1_clk_p] ;# MGTREFCLK0P_226
#set_property -dict {LOC AM9 } [get_ports pcie_refclk1_clk_n] ;# MGTREFCLK0N_226
#set_property -dict {LOC AJ18  IOSTANDARD LVCMOS18 PULLUP true} [get_ports pcie_rst_n1]
# 100 MHz MGT reference clock
create_clock -period 10.000 -name pcie_mgt_refclk [get_ports pcie_refclk_p]
#create_clock -period 10 -name pcie_mgt_refclk [get_ports pcie_refclk1_clk_p]

set_false_path -from [get_ports pcie_rst_n]
set_input_delay 0.000 [get_ports pcie_rst_n]

#set_false_path -from [get_ports {pcie_rst_n1}]
#set_input_delay 0 [get_ports {pcie_rst_n1}]

#set_property CLOCK_DEDICATED_ROUTE FALSE [get_nets zynq_inst/design_2_i/util_ds_buf_0/U0/IBUF_OUT[0]]