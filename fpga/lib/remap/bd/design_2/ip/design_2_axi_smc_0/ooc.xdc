# aclk {FREQ_HZ 250000000 CLK_DOMAIN design_2_xdma_0_0_axi_aclk PHASE 0.000} aclk1 {FREQ_HZ 250000000 CLK_DOMAIN design_2_zynq_ultra_ps_e_0_0_pl_clk0 PHASE 0.000}
# Clock Domain: design_2_xdma_0_0_axi_aclk
create_clock -name aclk -period 4.000 [get_ports aclk]
# Clock Domain: design_2_zynq_ultra_ps_e_0_0_pl_clk0
create_clock -name aclk1 -period 4.000 [get_ports aclk1]
# Generated clocks
