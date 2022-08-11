// Copyright 1986-2020 Xilinx, Inc. All Rights Reserved.
// --------------------------------------------------------------------------------
// Tool Version: Vivado v.2020.2 (lin64) Build 3064766 Wed Nov 18 09:12:47 MST 2020
// Date        : Sun Feb 27 21:29:02 2022
// Host        : hemanthr-ssrg running 64-bit Ubuntu 18.04.6 LTS
// Command     : write_verilog -force -mode synth_stub
//               /home/hemanthr/corundum_copy/fpga/mqnic/250S0C/fpga_pcie/fpga/fpga.gen/sources_1/ip/pcie4_uscale_plus_0/pcie4_uscale_plus_0_stub.v
// Design      : pcie4_uscale_plus_0
// Purpose     : Stub declaration of top-level module interface
// Device      : xczu19eg-ffvd1760-2-e
// --------------------------------------------------------------------------------

// This empty module with port declaration file causes synthesis tools to infer a black box for IP.
// The synthesis directives are for Synopsys Synplify support to prevent IO buffer insertion.
// Please paste the declaration into a Verilog source file or add the file as an additional source.
(* X_CORE_INFO = "pcie4_uscale_plus_0_pcie4_uscale_core_top,Vivado 2020.2" *)
module pcie4_uscale_plus_0(pci_exp_txn, pci_exp_txp, pci_exp_rxn, 
  pci_exp_rxp, user_clk, user_reset, user_lnk_up, s_axis_rq_tdata, s_axis_rq_tkeep, 
  s_axis_rq_tlast, s_axis_rq_tready, s_axis_rq_tuser, s_axis_rq_tvalid, m_axis_rc_tdata, 
  m_axis_rc_tkeep, m_axis_rc_tlast, m_axis_rc_tready, m_axis_rc_tuser, m_axis_rc_tvalid, 
  m_axis_cq_tdata, m_axis_cq_tkeep, m_axis_cq_tlast, m_axis_cq_tready, m_axis_cq_tuser, 
  m_axis_cq_tvalid, s_axis_cc_tdata, s_axis_cc_tkeep, s_axis_cc_tlast, s_axis_cc_tready, 
  s_axis_cc_tuser, s_axis_cc_tvalid, pcie_rq_seq_num0, pcie_rq_seq_num_vld0, 
  pcie_rq_seq_num1, pcie_rq_seq_num_vld1, pcie_rq_tag0, pcie_rq_tag1, pcie_rq_tag_av, 
  pcie_rq_tag_vld0, pcie_rq_tag_vld1, pcie_tfc_nph_av, pcie_tfc_npd_av, pcie_cq_np_req, 
  pcie_cq_np_req_count, cfg_phy_link_down, cfg_phy_link_status, cfg_negotiated_width, 
  cfg_current_speed, cfg_max_payload, cfg_max_read_req, cfg_function_status, 
  cfg_function_power_state, cfg_vf_status, cfg_vf_power_state, cfg_link_power_state, 
  cfg_mgmt_addr, cfg_mgmt_function_number, cfg_mgmt_write, cfg_mgmt_write_data, 
  cfg_mgmt_byte_enable, cfg_mgmt_read, cfg_mgmt_read_data, cfg_mgmt_read_write_done, 
  cfg_mgmt_debug_access, cfg_err_cor_out, cfg_err_nonfatal_out, cfg_err_fatal_out, 
  cfg_local_error_valid, cfg_local_error_out, cfg_ltssm_state, cfg_rx_pm_state, 
  cfg_tx_pm_state, cfg_rcb_status, cfg_obff_enable, cfg_pl_status_change, 
  cfg_tph_requester_enable, cfg_tph_st_mode, cfg_vf_tph_requester_enable, 
  cfg_vf_tph_st_mode, cfg_msg_received, cfg_msg_received_data, cfg_msg_received_type, 
  cfg_msg_transmit, cfg_msg_transmit_type, cfg_msg_transmit_data, cfg_msg_transmit_done, 
  cfg_fc_ph, cfg_fc_pd, cfg_fc_nph, cfg_fc_npd, cfg_fc_cplh, cfg_fc_cpld, cfg_fc_sel, cfg_dsn, 
  cfg_bus_number, cfg_power_state_change_ack, cfg_power_state_change_interrupt, 
  cfg_err_cor_in, cfg_err_uncor_in, cfg_flr_in_process, cfg_flr_done, 
  cfg_vf_flr_in_process, cfg_vf_flr_func_num, cfg_vf_flr_done, cfg_link_training_enable, 
  cfg_interrupt_int, cfg_interrupt_pending, cfg_interrupt_sent, cfg_interrupt_msi_enable, 
  cfg_interrupt_msi_mmenable, cfg_interrupt_msi_mask_update, cfg_interrupt_msi_data, 
  cfg_interrupt_msi_select, cfg_interrupt_msi_int, cfg_interrupt_msi_pending_status, 
  cfg_interrupt_msi_pending_status_data_enable, 
  cfg_interrupt_msi_pending_status_function_num, cfg_interrupt_msi_sent, 
  cfg_interrupt_msi_fail, cfg_interrupt_msi_attr, cfg_interrupt_msi_tph_present, 
  cfg_interrupt_msi_tph_type, cfg_interrupt_msi_tph_st_tag, 
  cfg_interrupt_msi_function_number, cfg_pm_aspm_l1_entry_reject, 
  cfg_pm_aspm_tx_l0s_entry_disable, cfg_hot_reset_out, cfg_config_space_enable, 
  cfg_req_pm_transition_l23_ready, cfg_hot_reset_in, cfg_ds_port_number, 
  cfg_ds_bus_number, cfg_ds_device_number, sys_clk, sys_clk_gt, sys_reset, phy_rdy_out)
/* synthesis syn_black_box black_box_pad_pin="pci_exp_txn[15:0],pci_exp_txp[15:0],pci_exp_rxn[15:0],pci_exp_rxp[15:0],user_clk,user_reset,user_lnk_up,s_axis_rq_tdata[511:0],s_axis_rq_tkeep[15:0],s_axis_rq_tlast,s_axis_rq_tready[3:0],s_axis_rq_tuser[136:0],s_axis_rq_tvalid,m_axis_rc_tdata[511:0],m_axis_rc_tkeep[15:0],m_axis_rc_tlast,m_axis_rc_tready,m_axis_rc_tuser[160:0],m_axis_rc_tvalid,m_axis_cq_tdata[511:0],m_axis_cq_tkeep[15:0],m_axis_cq_tlast,m_axis_cq_tready,m_axis_cq_tuser[182:0],m_axis_cq_tvalid,s_axis_cc_tdata[511:0],s_axis_cc_tkeep[15:0],s_axis_cc_tlast,s_axis_cc_tready[3:0],s_axis_cc_tuser[80:0],s_axis_cc_tvalid,pcie_rq_seq_num0[5:0],pcie_rq_seq_num_vld0,pcie_rq_seq_num1[5:0],pcie_rq_seq_num_vld1,pcie_rq_tag0[7:0],pcie_rq_tag1[7:0],pcie_rq_tag_av[3:0],pcie_rq_tag_vld0,pcie_rq_tag_vld1,pcie_tfc_nph_av[3:0],pcie_tfc_npd_av[3:0],pcie_cq_np_req[1:0],pcie_cq_np_req_count[5:0],cfg_phy_link_down,cfg_phy_link_status[1:0],cfg_negotiated_width[2:0],cfg_current_speed[1:0],cfg_max_payload[1:0],cfg_max_read_req[2:0],cfg_function_status[15:0],cfg_function_power_state[11:0],cfg_vf_status[503:0],cfg_vf_power_state[755:0],cfg_link_power_state[1:0],cfg_mgmt_addr[9:0],cfg_mgmt_function_number[7:0],cfg_mgmt_write,cfg_mgmt_write_data[31:0],cfg_mgmt_byte_enable[3:0],cfg_mgmt_read,cfg_mgmt_read_data[31:0],cfg_mgmt_read_write_done,cfg_mgmt_debug_access,cfg_err_cor_out,cfg_err_nonfatal_out,cfg_err_fatal_out,cfg_local_error_valid,cfg_local_error_out[4:0],cfg_ltssm_state[5:0],cfg_rx_pm_state[1:0],cfg_tx_pm_state[1:0],cfg_rcb_status[3:0],cfg_obff_enable[1:0],cfg_pl_status_change,cfg_tph_requester_enable[3:0],cfg_tph_st_mode[11:0],cfg_vf_tph_requester_enable[251:0],cfg_vf_tph_st_mode[755:0],cfg_msg_received,cfg_msg_received_data[7:0],cfg_msg_received_type[4:0],cfg_msg_transmit,cfg_msg_transmit_type[2:0],cfg_msg_transmit_data[31:0],cfg_msg_transmit_done,cfg_fc_ph[7:0],cfg_fc_pd[11:0],cfg_fc_nph[7:0],cfg_fc_npd[11:0],cfg_fc_cplh[7:0],cfg_fc_cpld[11:0],cfg_fc_sel[2:0],cfg_dsn[63:0],cfg_bus_number[7:0],cfg_power_state_change_ack,cfg_power_state_change_interrupt,cfg_err_cor_in,cfg_err_uncor_in,cfg_flr_in_process[3:0],cfg_flr_done[3:0],cfg_vf_flr_in_process[251:0],cfg_vf_flr_func_num[7:0],cfg_vf_flr_done[0:0],cfg_link_training_enable,cfg_interrupt_int[3:0],cfg_interrupt_pending[3:0],cfg_interrupt_sent,cfg_interrupt_msi_enable[3:0],cfg_interrupt_msi_mmenable[11:0],cfg_interrupt_msi_mask_update,cfg_interrupt_msi_data[31:0],cfg_interrupt_msi_select[1:0],cfg_interrupt_msi_int[31:0],cfg_interrupt_msi_pending_status[31:0],cfg_interrupt_msi_pending_status_data_enable,cfg_interrupt_msi_pending_status_function_num[1:0],cfg_interrupt_msi_sent,cfg_interrupt_msi_fail,cfg_interrupt_msi_attr[2:0],cfg_interrupt_msi_tph_present,cfg_interrupt_msi_tph_type[1:0],cfg_interrupt_msi_tph_st_tag[7:0],cfg_interrupt_msi_function_number[7:0],cfg_pm_aspm_l1_entry_reject,cfg_pm_aspm_tx_l0s_entry_disable,cfg_hot_reset_out,cfg_config_space_enable,cfg_req_pm_transition_l23_ready,cfg_hot_reset_in,cfg_ds_port_number[7:0],cfg_ds_bus_number[7:0],cfg_ds_device_number[4:0],sys_clk,sys_clk_gt,sys_reset,phy_rdy_out" */;
  output [15:0]pci_exp_txn;
  output [15:0]pci_exp_txp;
  input [15:0]pci_exp_rxn;
  input [15:0]pci_exp_rxp;
  output user_clk;
  output user_reset;
  output user_lnk_up;
  input [511:0]s_axis_rq_tdata;
  input [15:0]s_axis_rq_tkeep;
  input s_axis_rq_tlast;
  output [3:0]s_axis_rq_tready;
  input [136:0]s_axis_rq_tuser;
  input s_axis_rq_tvalid;
  output [511:0]m_axis_rc_tdata;
  output [15:0]m_axis_rc_tkeep;
  output m_axis_rc_tlast;
  input m_axis_rc_tready;
  output [160:0]m_axis_rc_tuser;
  output m_axis_rc_tvalid;
  output [511:0]m_axis_cq_tdata;
  output [15:0]m_axis_cq_tkeep;
  output m_axis_cq_tlast;
  input m_axis_cq_tready;
  output [182:0]m_axis_cq_tuser;
  output m_axis_cq_tvalid;
  input [511:0]s_axis_cc_tdata;
  input [15:0]s_axis_cc_tkeep;
  input s_axis_cc_tlast;
  output [3:0]s_axis_cc_tready;
  input [80:0]s_axis_cc_tuser;
  input s_axis_cc_tvalid;
  output [5:0]pcie_rq_seq_num0;
  output pcie_rq_seq_num_vld0;
  output [5:0]pcie_rq_seq_num1;
  output pcie_rq_seq_num_vld1;
  output [7:0]pcie_rq_tag0;
  output [7:0]pcie_rq_tag1;
  output [3:0]pcie_rq_tag_av;
  output pcie_rq_tag_vld0;
  output pcie_rq_tag_vld1;
  output [3:0]pcie_tfc_nph_av;
  output [3:0]pcie_tfc_npd_av;
  input [1:0]pcie_cq_np_req;
  output [5:0]pcie_cq_np_req_count;
  output cfg_phy_link_down;
  output [1:0]cfg_phy_link_status;
  output [2:0]cfg_negotiated_width;
  output [1:0]cfg_current_speed;
  output [1:0]cfg_max_payload;
  output [2:0]cfg_max_read_req;
  output [15:0]cfg_function_status;
  output [11:0]cfg_function_power_state;
  output [503:0]cfg_vf_status;
  output [755:0]cfg_vf_power_state;
  output [1:0]cfg_link_power_state;
  input [9:0]cfg_mgmt_addr;
  input [7:0]cfg_mgmt_function_number;
  input cfg_mgmt_write;
  input [31:0]cfg_mgmt_write_data;
  input [3:0]cfg_mgmt_byte_enable;
  input cfg_mgmt_read;
  output [31:0]cfg_mgmt_read_data;
  output cfg_mgmt_read_write_done;
  input cfg_mgmt_debug_access;
  output cfg_err_cor_out;
  output cfg_err_nonfatal_out;
  output cfg_err_fatal_out;
  output cfg_local_error_valid;
  output [4:0]cfg_local_error_out;
  output [5:0]cfg_ltssm_state;
  output [1:0]cfg_rx_pm_state;
  output [1:0]cfg_tx_pm_state;
  output [3:0]cfg_rcb_status;
  output [1:0]cfg_obff_enable;
  output cfg_pl_status_change;
  output [3:0]cfg_tph_requester_enable;
  output [11:0]cfg_tph_st_mode;
  output [251:0]cfg_vf_tph_requester_enable;
  output [755:0]cfg_vf_tph_st_mode;
  output cfg_msg_received;
  output [7:0]cfg_msg_received_data;
  output [4:0]cfg_msg_received_type;
  input cfg_msg_transmit;
  input [2:0]cfg_msg_transmit_type;
  input [31:0]cfg_msg_transmit_data;
  output cfg_msg_transmit_done;
  output [7:0]cfg_fc_ph;
  output [11:0]cfg_fc_pd;
  output [7:0]cfg_fc_nph;
  output [11:0]cfg_fc_npd;
  output [7:0]cfg_fc_cplh;
  output [11:0]cfg_fc_cpld;
  input [2:0]cfg_fc_sel;
  input [63:0]cfg_dsn;
  output [7:0]cfg_bus_number;
  input cfg_power_state_change_ack;
  output cfg_power_state_change_interrupt;
  input cfg_err_cor_in;
  input cfg_err_uncor_in;
  output [3:0]cfg_flr_in_process;
  input [3:0]cfg_flr_done;
  output [251:0]cfg_vf_flr_in_process;
  input [7:0]cfg_vf_flr_func_num;
  input [0:0]cfg_vf_flr_done;
  input cfg_link_training_enable;
  input [3:0]cfg_interrupt_int;
  input [3:0]cfg_interrupt_pending;
  output cfg_interrupt_sent;
  output [3:0]cfg_interrupt_msi_enable;
  output [11:0]cfg_interrupt_msi_mmenable;
  output cfg_interrupt_msi_mask_update;
  output [31:0]cfg_interrupt_msi_data;
  input [1:0]cfg_interrupt_msi_select;
  input [31:0]cfg_interrupt_msi_int;
  input [31:0]cfg_interrupt_msi_pending_status;
  input cfg_interrupt_msi_pending_status_data_enable;
  input [1:0]cfg_interrupt_msi_pending_status_function_num;
  output cfg_interrupt_msi_sent;
  output cfg_interrupt_msi_fail;
  input [2:0]cfg_interrupt_msi_attr;
  input cfg_interrupt_msi_tph_present;
  input [1:0]cfg_interrupt_msi_tph_type;
  input [7:0]cfg_interrupt_msi_tph_st_tag;
  input [7:0]cfg_interrupt_msi_function_number;
  input cfg_pm_aspm_l1_entry_reject;
  input cfg_pm_aspm_tx_l0s_entry_disable;
  output cfg_hot_reset_out;
  input cfg_config_space_enable;
  input cfg_req_pm_transition_l23_ready;
  input cfg_hot_reset_in;
  input [7:0]cfg_ds_port_number;
  input [7:0]cfg_ds_bus_number;
  input [4:0]cfg_ds_device_number;
  input sys_clk;
  input sys_clk_gt;
  input sys_reset;
  output phy_rdy_out;
endmodule
