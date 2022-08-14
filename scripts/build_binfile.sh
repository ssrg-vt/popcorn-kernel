#! /bin/sh
# This script should be copied into the top level project directory and run after running
# petalinux-build
# It specifies the FPGA bitfile, first stage bootloader which loads the bitfile from flash at power on, u-boot bootloader,  pmufw and linux kernel
# The linux kernel offset is calculated from the flash offsets which are set by running
# petalinux-config --get-hw-description=<path to HDF file>
#
# Subsystem AUTO Hardware Settings  --->
# Flash Settings  ---> 
#     Primary Flash (psu_qspi_0)  --->                                                                                                                 
#     Advanced Flash Auto Configuration                                                                                                               
#     *** partition 0 ***                                                                                                                              
#     (boot) name                                                                                                                                           
#     (0x200000) size                                                                                                                                      
#     *** partition 1 ***                                                                                                                            
#     (bootenv) name 
#     (0x40000) size
#     *** partition 2 ***
#     (bitfile) name                                                                                                                                       
#     (0x1C00000) size                                                                                                                                   
#     *** partition 3 ***                                                                                                                             
#     (kernel) name                                                                                                                                       
#     (0x06040000) size                                                                                                                                  
#     *** partition 4 ***                                                                                                                              
#     (bootscr)  name
#     (0x00180000) size    
##########################
# Kernel offset = partition0 size + partition1 size + partition2 size
# Which in this case = 0x100000 + 0x40000 + 0x1C00000 = 0x1D40000
# The flash size = 0x8000000
# partition3 size = flash size - Kernel offset = 0x8000000 - 0x1D40000 = 0x62C0000
KERNEL_OFFSET=0x1E40000
BOOT_SCR=0x7E80000
echo "Kernel offset set to ${KERNEL_OFFSET}, make sure this is correct (see explanation in comments in this script)"
petalinux-package --boot --fsbl images/linux/zynqmp_fsbl.elf --fpga images/linux/system.bit --u-boot images/linux/u-boot.elf --pmufw images/linux/pmufw.elf --kernel images/linux/image.ub --offset ${KERNEL_OFFSET} --add images/linux/boot.scr --offset ${BOOT_SCR} --force
