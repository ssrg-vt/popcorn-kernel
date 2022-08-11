# Definitional proc to organize widgets for parameters.
proc init_gui { IPINST } {
  #Adding Page
  set BAR_Options [ipgui::add_page $IPINST -name "BAR Options"]
  ipgui::add_param $IPINST -name "Component_Name" -parent ${BAR_Options}
  #Adding Group
  set BAR_0 [ipgui::add_group $IPINST -name "BAR 0" -parent ${BAR_Options}]
  ipgui::add_param $IPINST -name "BAR0SIZE" -parent ${BAR_0}
  ipgui::add_param $IPINST -name "BAR2AXI0_TRANSLATION" -parent ${BAR_0}

  #Adding Group
  set BAR_1 [ipgui::add_group $IPINST -name "BAR 1" -parent ${BAR_Options}]
  ipgui::add_param $IPINST -name "BAR1SIZE" -parent ${BAR_1}
  ipgui::add_param $IPINST -name "BAR2AXI1_TRANSLATION" -parent ${BAR_1}

  #Adding Group
  set BAR_2 [ipgui::add_group $IPINST -name "BAR 2" -parent ${BAR_Options}]
  ipgui::add_param $IPINST -name "BAR2SIZE" -parent ${BAR_2}
  ipgui::add_param $IPINST -name "BAR2AXI2_TRANSLATION" -parent ${BAR_2}

  #Adding Group
  set BAR_3 [ipgui::add_group $IPINST -name "BAR 3" -parent ${BAR_Options}]
  ipgui::add_param $IPINST -name "BAR3SIZE" -parent ${BAR_3}
  ipgui::add_param $IPINST -name "BAR2AXI3_TRANSLATION" -parent ${BAR_3}

  #Adding Group
  set BAR_4 [ipgui::add_group $IPINST -name "BAR 4" -parent ${BAR_Options}]
  ipgui::add_param $IPINST -name "BAR4SIZE" -parent ${BAR_4}
  ipgui::add_param $IPINST -name "BAR2AXI4_TRANSLATION" -parent ${BAR_4}

  #Adding Group
  set BAR_5 [ipgui::add_group $IPINST -name "BAR 5" -parent ${BAR_Options}]
  ipgui::add_param $IPINST -name "BAR5SIZE" -parent ${BAR_5}
  ipgui::add_param $IPINST -name "BAR2AXI5_TRANSLATION" -parent ${BAR_5}


  #Adding Page
  set Data_Width_Options [ipgui::add_page $IPINST -name "Data Width Options"]
  ipgui::add_param $IPINST -name "M_AXI_ADDR_WIDTH" -parent ${Data_Width_Options}
  ipgui::add_param $IPINST -name "AXIS_TDATA_WIDTH" -parent ${Data_Width_Options} -widget comboBox

  #Adding Page
  set Misc [ipgui::add_page $IPINST -name "Misc"]
  ipgui::add_param $IPINST -name "ENABLE_CONFIG" -parent ${Misc}
  ipgui::add_param $IPINST -name "RELAXED_ORDERING" -parent ${Misc}
  ipgui::add_param $IPINST -name "OUTSTANDING_READS" -parent ${Misc} -widget comboBox


}

proc update_PARAM_VALUE.BAR0SIZE { PARAM_VALUE.BAR0SIZE } {
	# Procedure called to update BAR0SIZE when any of the dependent parameters in the arguments change
}

proc validate_PARAM_VALUE.BAR0SIZE { PARAM_VALUE.BAR0SIZE } {
	# Procedure called to validate BAR0SIZE
	return true
}

proc update_PARAM_VALUE.BAR2AXI0_TRANSLATION { PARAM_VALUE.BAR2AXI0_TRANSLATION } {
	# Procedure called to update BAR2AXI0_TRANSLATION when any of the dependent parameters in the arguments change
}

proc validate_PARAM_VALUE.BAR2AXI0_TRANSLATION { PARAM_VALUE.BAR2AXI0_TRANSLATION } {
	# Procedure called to validate BAR2AXI0_TRANSLATION
	return true
}

proc update_PARAM_VALUE.BAR1SIZE { PARAM_VALUE.BAR1SIZE } {
	# Procedure called to update BAR1SIZE when any of the dependent parameters in the arguments change
}

proc validate_PARAM_VALUE.BAR1SIZE { PARAM_VALUE.BAR1SIZE } {
	# Procedure called to validate BAR1SIZE
	return true
}

proc update_PARAM_VALUE.BAR2AXI1_TRANSLATION { PARAM_VALUE.BAR2AXI1_TRANSLATION } {
	# Procedure called to update BAR2AXI1_TRANSLATION when any of the dependent parameters in the arguments change
}

proc validate_PARAM_VALUE.BAR2AXI1_TRANSLATION { PARAM_VALUE.BAR2AXI1_TRANSLATION } {
	# Procedure called to validate BAR2AXI1_TRANSLATION
	return true
}

proc update_PARAM_VALUE.BAR2SIZE { PARAM_VALUE.BAR2SIZE } {
	# Procedure called to update BAR2SIZE when any of the dependent parameters in the arguments change
}

proc validate_PARAM_VALUE.BAR2SIZE { PARAM_VALUE.BAR2SIZE } {
	# Procedure called to validate BAR2SIZE
	return true
}

proc update_PARAM_VALUE.BAR2AXI2_TRANSLATION { PARAM_VALUE.BAR2AXI2_TRANSLATION } {
	# Procedure called to update BAR2AXI2_TRANSLATION when any of the dependent parameters in the arguments change
}

proc validate_PARAM_VALUE.BAR2AXI2_TRANSLATION { PARAM_VALUE.BAR2AXI2_TRANSLATION } {
	# Procedure called to validate BAR2AXI2_TRANSLATION
	return true
}

proc update_PARAM_VALUE.BAR3SIZE { PARAM_VALUE.BAR3SIZE } {
	# Procedure called to update BAR3SIZE when any of the dependent parameters in the arguments change
}

proc validate_PARAM_VALUE.BAR3SIZE { PARAM_VALUE.BAR3SIZE } {
	# Procedure called to validate BAR3SIZE
	return true
}

proc update_PARAM_VALUE.BAR2AXI3_TRANSLATION { PARAM_VALUE.BAR2AXI3_TRANSLATION } {
	# Procedure called to update BAR2AXI3_TRANSLATION when any of the dependent parameters in the arguments change
}

proc validate_PARAM_VALUE.BAR2AXI3_TRANSLATION { PARAM_VALUE.BAR2AXI3_TRANSLATION } {
	# Procedure called to validate BAR2AXI3_TRANSLATION
	return true
}

proc update_PARAM_VALUE.BAR4SIZE { PARAM_VALUE.BAR4SIZE } {
	# Procedure called to update BAR4SIZE when any of the dependent parameters in the arguments change
}

proc validate_PARAM_VALUE.BAR4SIZE { PARAM_VALUE.BAR4SIZE } {
	# Procedure called to validate BAR4SIZE
	return true
}

proc update_PARAM_VALUE.BAR2AXI4_TRANSLATION { PARAM_VALUE.BAR2AXI4_TRANSLATION } {
	# Procedure called to update BAR2AXI4_TRANSLATION when any of the dependent parameters in the arguments change
}

proc validate_PARAM_VALUE.BAR2AXI4_TRANSLATION { PARAM_VALUE.BAR2AXI4_TRANSLATION } {
	# Procedure called to validate BAR2AXI4_TRANSLATION
	return true
}

proc update_PARAM_VALUE.BAR5SIZE { PARAM_VALUE.BAR5SIZE } {
	# Procedure called to update BAR5SIZE when any of the dependent parameters in the arguments change
}

proc validate_PARAM_VALUE.BAR5SIZE { PARAM_VALUE.BAR5SIZE } {
	# Procedure called to validate BAR5SIZE
	return true
}

proc update_PARAM_VALUE.BAR2AXI5_TRANSLATION { PARAM_VALUE.BAR2AXI5_TRANSLATION } {
	# Procedure called to update BAR2AXI5_TRANSLATION when any of the dependent parameters in the arguments change
}

proc validate_PARAM_VALUE.BAR2AXI5_TRANSLATION { PARAM_VALUE.BAR2AXI5_TRANSLATION } {
	# Procedure called to validate BAR2AXI5_TRANSLATION
	return true
}

proc update_PARAM_VALUE.M_AXI_ADDR_WIDTH { PARAM_VALUE.M_AXI_ADDR_WIDTH } {
	# Procedure called to update M_AXI_ADDR_WIDTH when any of the dependent parameters in the arguments change
}

proc validate_PARAM_VALUE.M_AXI_ADDR_WIDTH { PARAM_VALUE.M_AXI_ADDR_WIDTH } {
	# Procedure called to validate M_AXI_ADDR_WIDTH
	return true
}

proc update_PARAM_VALUE.AXIS_TDATA_WIDTH { PARAM_VALUE.AXIS_TDATA_WIDTH } {
	# Procedure called to update AXIS_TDATA_WIDTH when any of the dependent parameters in the arguments change
}

proc validate_PARAM_VALUE.AXIS_TDATA_WIDTH { PARAM_VALUE.AXIS_TDATA_WIDTH } {
	# Procedure called to validate AXIS_TDATA_WIDTH
	return true
}

proc update_PARAM_VALUE.ENABLE_CONFIG { PARAM_VALUE.ENABLE_CONFIG } {
	# Procedure called to update ENABLE_CONFIG when any of the dependent parameters in the arguments change
}

proc validate_PARAM_VALUE.ENABLE_CONFIG { PARAM_VALUE.ENABLE_CONFIG } {
	# Procedure called to validate ENABLE_CONFIG
	return true
}

proc update_PARAM_VALUE.RELAXED_ORDERING { PARAM_VALUE.RELAXED_ORDERING } {
	# Procedure called to update RELAXED_ORDERING when any of the dependent parameters in the arguments change
}

proc validate_PARAM_VALUE.RELAXED_ORDERING { PARAM_VALUE.RELAXED_ORDERING } {
	# Procedure called to validate RELAXED_ORDERING
	return true
}

proc update_PARAM_VALUE.OUTSTANDING_READS { PARAM_VALUE.OUTSTANDING_READS } {
	# Procedure called to update OUTSTANDING_READS when any of the dependent parameters in the arguments change
}

proc validate_PARAM_VALUE.OUTSTANDING_READS { PARAM_VALUE.OUTSTANDING_READS } {
	# Procedure called to validate OUTSTANDING_READS
	return true
}


proc update_MODELPARAM_VALUE.AXIS_TDATA_WIDTH { MODELPARAM_VALUE.AXIS_TDATA_WIDTH PARAM_VALUE.AXIS_TDATA_WIDTH } {
	# Procedure called to set VHDL generic/Verilog parameter value(s) based on TCL parameter value
	set_property value [get_property value ${PARAM_VALUE.AXIS_TDATA_WIDTH}] ${MODELPARAM_VALUE.AXIS_TDATA_WIDTH}
}

proc update_MODELPARAM_VALUE.S_AXI_TDATA_WIDTH { MODELPARAM_VALUE.S_AXI_TDATA_WIDTH } {
	# Procedure called to set VHDL generic/Verilog parameter value(s) based on TCL parameter value
	# WARNING: There is no corresponding user parameter named "S_AXI_TDATA_WIDTH". Setting updated value from the model parameter.
set_property value 32 ${MODELPARAM_VALUE.S_AXI_TDATA_WIDTH}
}

proc update_MODELPARAM_VALUE.S_AXI_ADDR_WIDTH { MODELPARAM_VALUE.S_AXI_ADDR_WIDTH } {
	# Procedure called to set VHDL generic/Verilog parameter value(s) based on TCL parameter value
	# WARNING: There is no corresponding user parameter named "S_AXI_ADDR_WIDTH". Setting updated value from the model parameter.
set_property value 5 ${MODELPARAM_VALUE.S_AXI_ADDR_WIDTH}
}

proc update_MODELPARAM_VALUE.M_AXI_TDATA_WIDTH { MODELPARAM_VALUE.M_AXI_TDATA_WIDTH } {
	# Procedure called to set VHDL generic/Verilog parameter value(s) based on TCL parameter value
	# WARNING: There is no corresponding user parameter named "M_AXI_TDATA_WIDTH". Setting updated value from the model parameter.
set_property value 64 ${MODELPARAM_VALUE.M_AXI_TDATA_WIDTH}
}

proc update_MODELPARAM_VALUE.M_AXI_ADDR_WIDTH { MODELPARAM_VALUE.M_AXI_ADDR_WIDTH PARAM_VALUE.M_AXI_ADDR_WIDTH } {
	# Procedure called to set VHDL generic/Verilog parameter value(s) based on TCL parameter value
	set_property value [get_property value ${PARAM_VALUE.M_AXI_ADDR_WIDTH}] ${MODELPARAM_VALUE.M_AXI_ADDR_WIDTH}
}

proc update_MODELPARAM_VALUE.RELAXED_ORDERING { MODELPARAM_VALUE.RELAXED_ORDERING PARAM_VALUE.RELAXED_ORDERING } {
	# Procedure called to set VHDL generic/Verilog parameter value(s) based on TCL parameter value
	set_property value [get_property value ${PARAM_VALUE.RELAXED_ORDERING}] ${MODELPARAM_VALUE.RELAXED_ORDERING}
}

proc update_MODELPARAM_VALUE.ENABLE_CONFIG { MODELPARAM_VALUE.ENABLE_CONFIG PARAM_VALUE.ENABLE_CONFIG } {
	# Procedure called to set VHDL generic/Verilog parameter value(s) based on TCL parameter value
	set_property value [get_property value ${PARAM_VALUE.ENABLE_CONFIG}] ${MODELPARAM_VALUE.ENABLE_CONFIG}
}

proc update_MODELPARAM_VALUE.BAR2AXI0_TRANSLATION { MODELPARAM_VALUE.BAR2AXI0_TRANSLATION PARAM_VALUE.BAR2AXI0_TRANSLATION } {
	# Procedure called to set VHDL generic/Verilog parameter value(s) based on TCL parameter value
	set_property value [get_property value ${PARAM_VALUE.BAR2AXI0_TRANSLATION}] ${MODELPARAM_VALUE.BAR2AXI0_TRANSLATION}
}

proc update_MODELPARAM_VALUE.BAR2AXI1_TRANSLATION { MODELPARAM_VALUE.BAR2AXI1_TRANSLATION PARAM_VALUE.BAR2AXI1_TRANSLATION } {
	# Procedure called to set VHDL generic/Verilog parameter value(s) based on TCL parameter value
	set_property value [get_property value ${PARAM_VALUE.BAR2AXI1_TRANSLATION}] ${MODELPARAM_VALUE.BAR2AXI1_TRANSLATION}
}

proc update_MODELPARAM_VALUE.BAR2AXI2_TRANSLATION { MODELPARAM_VALUE.BAR2AXI2_TRANSLATION PARAM_VALUE.BAR2AXI2_TRANSLATION } {
	# Procedure called to set VHDL generic/Verilog parameter value(s) based on TCL parameter value
	set_property value [get_property value ${PARAM_VALUE.BAR2AXI2_TRANSLATION}] ${MODELPARAM_VALUE.BAR2AXI2_TRANSLATION}
}

proc update_MODELPARAM_VALUE.BAR2AXI3_TRANSLATION { MODELPARAM_VALUE.BAR2AXI3_TRANSLATION PARAM_VALUE.BAR2AXI3_TRANSLATION } {
	# Procedure called to set VHDL generic/Verilog parameter value(s) based on TCL parameter value
	set_property value [get_property value ${PARAM_VALUE.BAR2AXI3_TRANSLATION}] ${MODELPARAM_VALUE.BAR2AXI3_TRANSLATION}
}

proc update_MODELPARAM_VALUE.BAR2AXI4_TRANSLATION { MODELPARAM_VALUE.BAR2AXI4_TRANSLATION PARAM_VALUE.BAR2AXI4_TRANSLATION } {
	# Procedure called to set VHDL generic/Verilog parameter value(s) based on TCL parameter value
	set_property value [get_property value ${PARAM_VALUE.BAR2AXI4_TRANSLATION}] ${MODELPARAM_VALUE.BAR2AXI4_TRANSLATION}
}

proc update_MODELPARAM_VALUE.BAR2AXI5_TRANSLATION { MODELPARAM_VALUE.BAR2AXI5_TRANSLATION PARAM_VALUE.BAR2AXI5_TRANSLATION } {
	# Procedure called to set VHDL generic/Verilog parameter value(s) based on TCL parameter value
	set_property value [get_property value ${PARAM_VALUE.BAR2AXI5_TRANSLATION}] ${MODELPARAM_VALUE.BAR2AXI5_TRANSLATION}
}

proc update_MODELPARAM_VALUE.BAR0SIZE { MODELPARAM_VALUE.BAR0SIZE PARAM_VALUE.BAR0SIZE } {
	# Procedure called to set VHDL generic/Verilog parameter value(s) based on TCL parameter value
	set_property value [get_property value ${PARAM_VALUE.BAR0SIZE}] ${MODELPARAM_VALUE.BAR0SIZE}
}

proc update_MODELPARAM_VALUE.BAR1SIZE { MODELPARAM_VALUE.BAR1SIZE PARAM_VALUE.BAR1SIZE } {
	# Procedure called to set VHDL generic/Verilog parameter value(s) based on TCL parameter value
	set_property value [get_property value ${PARAM_VALUE.BAR1SIZE}] ${MODELPARAM_VALUE.BAR1SIZE}
}

proc update_MODELPARAM_VALUE.BAR2SIZE { MODELPARAM_VALUE.BAR2SIZE PARAM_VALUE.BAR2SIZE } {
	# Procedure called to set VHDL generic/Verilog parameter value(s) based on TCL parameter value
	set_property value [get_property value ${PARAM_VALUE.BAR2SIZE}] ${MODELPARAM_VALUE.BAR2SIZE}
}

proc update_MODELPARAM_VALUE.BAR3SIZE { MODELPARAM_VALUE.BAR3SIZE PARAM_VALUE.BAR3SIZE } {
	# Procedure called to set VHDL generic/Verilog parameter value(s) based on TCL parameter value
	set_property value [get_property value ${PARAM_VALUE.BAR3SIZE}] ${MODELPARAM_VALUE.BAR3SIZE}
}

proc update_MODELPARAM_VALUE.BAR4SIZE { MODELPARAM_VALUE.BAR4SIZE PARAM_VALUE.BAR4SIZE } {
	# Procedure called to set VHDL generic/Verilog parameter value(s) based on TCL parameter value
	set_property value [get_property value ${PARAM_VALUE.BAR4SIZE}] ${MODELPARAM_VALUE.BAR4SIZE}
}

proc update_MODELPARAM_VALUE.BAR5SIZE { MODELPARAM_VALUE.BAR5SIZE PARAM_VALUE.BAR5SIZE } {
	# Procedure called to set VHDL generic/Verilog parameter value(s) based on TCL parameter value
	set_property value [get_property value ${PARAM_VALUE.BAR5SIZE}] ${MODELPARAM_VALUE.BAR5SIZE}
}

proc update_MODELPARAM_VALUE.OUTSTANDING_READS { MODELPARAM_VALUE.OUTSTANDING_READS PARAM_VALUE.OUTSTANDING_READS } {
	# Procedure called to set VHDL generic/Verilog parameter value(s) based on TCL parameter value
	set_property value [get_property value ${PARAM_VALUE.OUTSTANDING_READS}] ${MODELPARAM_VALUE.OUTSTANDING_READS}
}

