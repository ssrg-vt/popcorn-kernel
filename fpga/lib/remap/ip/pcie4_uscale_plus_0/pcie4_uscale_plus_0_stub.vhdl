-- Copyright 1986-2020 Xilinx, Inc. All Rights Reserved.
-- --------------------------------------------------------------------------------
-- Tool Version: Vivado v.2020.2 (lin64) Build 3064766 Wed Nov 18 09:12:47 MST 2020
-- Date        : Sun Feb 27 21:29:02 2022
-- Host        : hemanthr-ssrg running 64-bit Ubuntu 18.04.6 LTS
-- Command     : write_vhdl -force -mode synth_stub
--               /home/hemanthr/corundum_copy/fpga/mqnic/250S0C/fpga_pcie/fpga/fpga.gen/sources_1/ip/pcie4_uscale_plus_0/pcie4_uscale_plus_0_stub.vhdl
-- Design      : pcie4_uscale_plus_0
-- Purpose     : Stub declaration of top-level module interface
-- Device      : xczu19eg-ffvd1760-2-e
-- --------------------------------------------------------------------------------
library IEEE;
use IEEE.STD_LOGIC_1164.ALL;

entity pcie4_uscale_plus_0 is
  Port ( 
    pci_exp_txn : out STD_LOGIC_VECTOR ( 15 downto 0 );
    pci_exp_txp : out STD_LOGIC_VECTOR ( 15 downto 0 );
    pci_exp_rxn : in STD_LOGIC_VECTOR ( 15 downto 0 );
    pci_exp_rxp : in STD_LOGIC_VECTOR ( 15 downto 0 );
    user_clk : out STD_LOGIC;
    user_reset : out STD_LOGIC;
    user_lnk_up : out STD_LOGIC;
    s_axis_rq_tdata : in STD_LOGIC_VECTOR ( 511 downto 0 );
    s_axis_rq_tkeep : in STD_LOGIC_VECTOR ( 15 downto 0 );
    s_axis_rq_tlast : in STD_LOGIC;
    s_axis_rq_tready : out STD_LOGIC_VECTOR ( 3 downto 0 );
    s_axis_rq_tuser : in STD_LOGIC_VECTOR ( 136 downto 0 );
    s_axis_rq_tvalid : in STD_LOGIC;
    m_axis_rc_tdata : out STD_LOGIC_VECTOR ( 511 downto 0 );
    m_axis_rc_tkeep : out STD_LOGIC_VECTOR ( 15 downto 0 );
    m_axis_rc_tlast : out STD_LOGIC;
    m_axis_rc_tready : in STD_LOGIC;
    m_axis_rc_tuser : out STD_LOGIC_VECTOR ( 160 downto 0 );
    m_axis_rc_tvalid : out STD_LOGIC;
    m_axis_cq_tdata : out STD_LOGIC_VECTOR ( 511 downto 0 );
    m_axis_cq_tkeep : out STD_LOGIC_VECTOR ( 15 downto 0 );
    m_axis_cq_tlast : out STD_LOGIC;
    m_axis_cq_tready : in STD_LOGIC;
    m_axis_cq_tuser : out STD_LOGIC_VECTOR ( 182 downto 0 );
    m_axis_cq_tvalid : out STD_LOGIC;
    s_axis_cc_tdata : in STD_LOGIC_VECTOR ( 511 downto 0 );
    s_axis_cc_tkeep : in STD_LOGIC_VECTOR ( 15 downto 0 );
    s_axis_cc_tlast : in STD_LOGIC;
    s_axis_cc_tready : out STD_LOGIC_VECTOR ( 3 downto 0 );
    s_axis_cc_tuser : in STD_LOGIC_VECTOR ( 80 downto 0 );
    s_axis_cc_tvalid : in STD_LOGIC;
    pcie_rq_seq_num0 : out STD_LOGIC_VECTOR ( 5 downto 0 );
    pcie_rq_seq_num_vld0 : out STD_LOGIC;
    pcie_rq_seq_num1 : out STD_LOGIC_VECTOR ( 5 downto 0 );
    pcie_rq_seq_num_vld1 : out STD_LOGIC;
    pcie_rq_tag0 : out STD_LOGIC_VECTOR ( 7 downto 0 );
    pcie_rq_tag1 : out STD_LOGIC_VECTOR ( 7 downto 0 );
    pcie_rq_tag_av : out STD_LOGIC_VECTOR ( 3 downto 0 );
    pcie_rq_tag_vld0 : out STD_LOGIC;
    pcie_rq_tag_vld1 : out STD_LOGIC;
    pcie_tfc_nph_av : out STD_LOGIC_VECTOR ( 3 downto 0 );
    pcie_tfc_npd_av : out STD_LOGIC_VECTOR ( 3 downto 0 );
    pcie_cq_np_req : in STD_LOGIC_VECTOR ( 1 downto 0 );
    pcie_cq_np_req_count : out STD_LOGIC_VECTOR ( 5 downto 0 );
    cfg_phy_link_down : out STD_LOGIC;
    cfg_phy_link_status : out STD_LOGIC_VECTOR ( 1 downto 0 );
    cfg_negotiated_width : out STD_LOGIC_VECTOR ( 2 downto 0 );
    cfg_current_speed : out STD_LOGIC_VECTOR ( 1 downto 0 );
    cfg_max_payload : out STD_LOGIC_VECTOR ( 1 downto 0 );
    cfg_max_read_req : out STD_LOGIC_VECTOR ( 2 downto 0 );
    cfg_function_status : out STD_LOGIC_VECTOR ( 15 downto 0 );
    cfg_function_power_state : out STD_LOGIC_VECTOR ( 11 downto 0 );
    cfg_vf_status : out STD_LOGIC_VECTOR ( 503 downto 0 );
    cfg_vf_power_state : out STD_LOGIC_VECTOR ( 755 downto 0 );
    cfg_link_power_state : out STD_LOGIC_VECTOR ( 1 downto 0 );
    cfg_mgmt_addr : in STD_LOGIC_VECTOR ( 9 downto 0 );
    cfg_mgmt_function_number : in STD_LOGIC_VECTOR ( 7 downto 0 );
    cfg_mgmt_write : in STD_LOGIC;
    cfg_mgmt_write_data : in STD_LOGIC_VECTOR ( 31 downto 0 );
    cfg_mgmt_byte_enable : in STD_LOGIC_VECTOR ( 3 downto 0 );
    cfg_mgmt_read : in STD_LOGIC;
    cfg_mgmt_read_data : out STD_LOGIC_VECTOR ( 31 downto 0 );
    cfg_mgmt_read_write_done : out STD_LOGIC;
    cfg_mgmt_debug_access : in STD_LOGIC;
    cfg_err_cor_out : out STD_LOGIC;
    cfg_err_nonfatal_out : out STD_LOGIC;
    cfg_err_fatal_out : out STD_LOGIC;
    cfg_local_error_valid : out STD_LOGIC;
    cfg_local_error_out : out STD_LOGIC_VECTOR ( 4 downto 0 );
    cfg_ltssm_state : out STD_LOGIC_VECTOR ( 5 downto 0 );
    cfg_rx_pm_state : out STD_LOGIC_VECTOR ( 1 downto 0 );
    cfg_tx_pm_state : out STD_LOGIC_VECTOR ( 1 downto 0 );
    cfg_rcb_status : out STD_LOGIC_VECTOR ( 3 downto 0 );
    cfg_obff_enable : out STD_LOGIC_VECTOR ( 1 downto 0 );
    cfg_pl_status_change : out STD_LOGIC;
    cfg_tph_requester_enable : out STD_LOGIC_VECTOR ( 3 downto 0 );
    cfg_tph_st_mode : out STD_LOGIC_VECTOR ( 11 downto 0 );
    cfg_vf_tph_requester_enable : out STD_LOGIC_VECTOR ( 251 downto 0 );
    cfg_vf_tph_st_mode : out STD_LOGIC_VECTOR ( 755 downto 0 );
    cfg_msg_received : out STD_LOGIC;
    cfg_msg_received_data : out STD_LOGIC_VECTOR ( 7 downto 0 );
    cfg_msg_received_type : out STD_LOGIC_VECTOR ( 4 downto 0 );
    cfg_msg_transmit : in STD_LOGIC;
    cfg_msg_transmit_type : in STD_LOGIC_VECTOR ( 2 downto 0 );
    cfg_msg_transmit_data : in STD_LOGIC_VECTOR ( 31 downto 0 );
    cfg_msg_transmit_done : out STD_LOGIC;
    cfg_fc_ph : out STD_LOGIC_VECTOR ( 7 downto 0 );
    cfg_fc_pd : out STD_LOGIC_VECTOR ( 11 downto 0 );
    cfg_fc_nph : out STD_LOGIC_VECTOR ( 7 downto 0 );
    cfg_fc_npd : out STD_LOGIC_VECTOR ( 11 downto 0 );
    cfg_fc_cplh : out STD_LOGIC_VECTOR ( 7 downto 0 );
    cfg_fc_cpld : out STD_LOGIC_VECTOR ( 11 downto 0 );
    cfg_fc_sel : in STD_LOGIC_VECTOR ( 2 downto 0 );
    cfg_dsn : in STD_LOGIC_VECTOR ( 63 downto 0 );
    cfg_bus_number : out STD_LOGIC_VECTOR ( 7 downto 0 );
    cfg_power_state_change_ack : in STD_LOGIC;
    cfg_power_state_change_interrupt : out STD_LOGIC;
    cfg_err_cor_in : in STD_LOGIC;
    cfg_err_uncor_in : in STD_LOGIC;
    cfg_flr_in_process : out STD_LOGIC_VECTOR ( 3 downto 0 );
    cfg_flr_done : in STD_LOGIC_VECTOR ( 3 downto 0 );
    cfg_vf_flr_in_process : out STD_LOGIC_VECTOR ( 251 downto 0 );
    cfg_vf_flr_func_num : in STD_LOGIC_VECTOR ( 7 downto 0 );
    cfg_vf_flr_done : in STD_LOGIC_VECTOR ( 0 to 0 );
    cfg_link_training_enable : in STD_LOGIC;
    cfg_interrupt_int : in STD_LOGIC_VECTOR ( 3 downto 0 );
    cfg_interrupt_pending : in STD_LOGIC_VECTOR ( 3 downto 0 );
    cfg_interrupt_sent : out STD_LOGIC;
    cfg_interrupt_msi_enable : out STD_LOGIC_VECTOR ( 3 downto 0 );
    cfg_interrupt_msi_mmenable : out STD_LOGIC_VECTOR ( 11 downto 0 );
    cfg_interrupt_msi_mask_update : out STD_LOGIC;
    cfg_interrupt_msi_data : out STD_LOGIC_VECTOR ( 31 downto 0 );
    cfg_interrupt_msi_select : in STD_LOGIC_VECTOR ( 1 downto 0 );
    cfg_interrupt_msi_int : in STD_LOGIC_VECTOR ( 31 downto 0 );
    cfg_interrupt_msi_pending_status : in STD_LOGIC_VECTOR ( 31 downto 0 );
    cfg_interrupt_msi_pending_status_data_enable : in STD_LOGIC;
    cfg_interrupt_msi_pending_status_function_num : in STD_LOGIC_VECTOR ( 1 downto 0 );
    cfg_interrupt_msi_sent : out STD_LOGIC;
    cfg_interrupt_msi_fail : out STD_LOGIC;
    cfg_interrupt_msi_attr : in STD_LOGIC_VECTOR ( 2 downto 0 );
    cfg_interrupt_msi_tph_present : in STD_LOGIC;
    cfg_interrupt_msi_tph_type : in STD_LOGIC_VECTOR ( 1 downto 0 );
    cfg_interrupt_msi_tph_st_tag : in STD_LOGIC_VECTOR ( 7 downto 0 );
    cfg_interrupt_msi_function_number : in STD_LOGIC_VECTOR ( 7 downto 0 );
    cfg_pm_aspm_l1_entry_reject : in STD_LOGIC;
    cfg_pm_aspm_tx_l0s_entry_disable : in STD_LOGIC;
    cfg_hot_reset_out : out STD_LOGIC;
    cfg_config_space_enable : in STD_LOGIC;
    cfg_req_pm_transition_l23_ready : in STD_LOGIC;
    cfg_hot_reset_in : in STD_LOGIC;
    cfg_ds_port_number : in STD_LOGIC_VECTOR ( 7 downto 0 );
    cfg_ds_bus_number : in STD_LOGIC_VECTOR ( 7 downto 0 );
    cfg_ds_device_number : in STD_LOGIC_VECTOR ( 4 downto 0 );
    sys_clk : in STD_LOGIC;
    sys_clk_gt : in STD_LOGIC;
    sys_reset : in STD_LOGIC;
    phy_rdy_out : out STD_LOGIC
  );

end pcie4_uscale_plus_0;

architecture stub of pcie4_uscale_plus_0 is
attribute syn_black_box : boolean;
attribute black_box_pad_pin : string;
attribute syn_black_box of stub : architecture is true;
attribute black_box_pad_pin of stub : architecture is "pci_exp_txn[15:0],pci_exp_txp[15:0],pci_exp_rxn[15:0],pci_exp_rxp[15:0],user_clk,user_reset,user_lnk_up,s_axis_rq_tdata[511:0],s_axis_rq_tkeep[15:0],s_axis_rq_tlast,s_axis_rq_tready[3:0],s_axis_rq_tuser[136:0],s_axis_rq_tvalid,m_axis_rc_tdata[511:0],m_axis_rc_tkeep[15:0],m_axis_rc_tlast,m_axis_rc_tready,m_axis_rc_tuser[160:0],m_axis_rc_tvalid,m_axis_cq_tdata[511:0],m_axis_cq_tkeep[15:0],m_axis_cq_tlast,m_axis_cq_tready,m_axis_cq_tuser[182:0],m_axis_cq_tvalid,s_axis_cc_tdata[511:0],s_axis_cc_tkeep[15:0],s_axis_cc_tlast,s_axis_cc_tready[3:0],s_axis_cc_tuser[80:0],s_axis_cc_tvalid,pcie_rq_seq_num0[5:0],pcie_rq_seq_num_vld0,pcie_rq_seq_num1[5:0],pcie_rq_seq_num_vld1,pcie_rq_tag0[7:0],pcie_rq_tag1[7:0],pcie_rq_tag_av[3:0],pcie_rq_tag_vld0,pcie_rq_tag_vld1,pcie_tfc_nph_av[3:0],pcie_tfc_npd_av[3:0],pcie_cq_np_req[1:0],pcie_cq_np_req_count[5:0],cfg_phy_link_down,cfg_phy_link_status[1:0],cfg_negotiated_width[2:0],cfg_current_speed[1:0],cfg_max_payload[1:0],cfg_max_read_req[2:0],cfg_function_status[15:0],cfg_function_power_state[11:0],cfg_vf_status[503:0],cfg_vf_power_state[755:0],cfg_link_power_state[1:0],cfg_mgmt_addr[9:0],cfg_mgmt_function_number[7:0],cfg_mgmt_write,cfg_mgmt_write_data[31:0],cfg_mgmt_byte_enable[3:0],cfg_mgmt_read,cfg_mgmt_read_data[31:0],cfg_mgmt_read_write_done,cfg_mgmt_debug_access,cfg_err_cor_out,cfg_err_nonfatal_out,cfg_err_fatal_out,cfg_local_error_valid,cfg_local_error_out[4:0],cfg_ltssm_state[5:0],cfg_rx_pm_state[1:0],cfg_tx_pm_state[1:0],cfg_rcb_status[3:0],cfg_obff_enable[1:0],cfg_pl_status_change,cfg_tph_requester_enable[3:0],cfg_tph_st_mode[11:0],cfg_vf_tph_requester_enable[251:0],cfg_vf_tph_st_mode[755:0],cfg_msg_received,cfg_msg_received_data[7:0],cfg_msg_received_type[4:0],cfg_msg_transmit,cfg_msg_transmit_type[2:0],cfg_msg_transmit_data[31:0],cfg_msg_transmit_done,cfg_fc_ph[7:0],cfg_fc_pd[11:0],cfg_fc_nph[7:0],cfg_fc_npd[11:0],cfg_fc_cplh[7:0],cfg_fc_cpld[11:0],cfg_fc_sel[2:0],cfg_dsn[63:0],cfg_bus_number[7:0],cfg_power_state_change_ack,cfg_power_state_change_interrupt,cfg_err_cor_in,cfg_err_uncor_in,cfg_flr_in_process[3:0],cfg_flr_done[3:0],cfg_vf_flr_in_process[251:0],cfg_vf_flr_func_num[7:0],cfg_vf_flr_done[0:0],cfg_link_training_enable,cfg_interrupt_int[3:0],cfg_interrupt_pending[3:0],cfg_interrupt_sent,cfg_interrupt_msi_enable[3:0],cfg_interrupt_msi_mmenable[11:0],cfg_interrupt_msi_mask_update,cfg_interrupt_msi_data[31:0],cfg_interrupt_msi_select[1:0],cfg_interrupt_msi_int[31:0],cfg_interrupt_msi_pending_status[31:0],cfg_interrupt_msi_pending_status_data_enable,cfg_interrupt_msi_pending_status_function_num[1:0],cfg_interrupt_msi_sent,cfg_interrupt_msi_fail,cfg_interrupt_msi_attr[2:0],cfg_interrupt_msi_tph_present,cfg_interrupt_msi_tph_type[1:0],cfg_interrupt_msi_tph_st_tag[7:0],cfg_interrupt_msi_function_number[7:0],cfg_pm_aspm_l1_entry_reject,cfg_pm_aspm_tx_l0s_entry_disable,cfg_hot_reset_out,cfg_config_space_enable,cfg_req_pm_transition_l23_ready,cfg_hot_reset_in,cfg_ds_port_number[7:0],cfg_ds_bus_number[7:0],cfg_ds_device_number[4:0],sys_clk,sys_clk_gt,sys_reset,phy_rdy_out";
attribute X_CORE_INFO : string;
attribute X_CORE_INFO of stub : architecture is "pcie4_uscale_plus_0_pcie4_uscale_core_top,Vivado 2020.2";
begin
end;
