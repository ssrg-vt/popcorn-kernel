NIC-fDSM 

This work enables the Petalinux running on the Smart NIC to communicate with the host operating system, while utilizing the FPGA as a NIC.

We have developed a hardware design that provides the datapath for the ARM cores on the Smart NIC (Bittware 250 SoC) to communicate with the host over PCIe and integrate this dataoath with the Corundum NIC (more details - https://github.com/corundum/corundum). 

We used Bittware 250 SoC in this work. The 250 SoC features a Xilinx Ultrascale+ Zynq MPSoC device featuring both the 64-bit ARM core processors and the programmable logic (PL). The ARM cores can communicate with the PL over AXI memory mapped interface. The Corundum NIC uses the Integrated block for PCIe IP from Xilinx, which instantiates the PCIe block in the FPGA to communicate with the host. We enable sharing of this PCIe core between Corundum and the ARM core. 

We have developed custom RTLs to convert the AXI-MM packets to PCIe TLPs and vice-versa. Then arbitrate the data packets from Corundum and the ARM core. 

File description:
1. fpga.v is the top level wrapper of the design and can be found in /fpga/mqnic/250S0C/fpga_pcie/rtl.
2. zynq_ps.tcl is the block design that contains the datapath between ARM core and the host and can be in fpga/mqnic/250S0C/fpga_pcie/ip.

Prerequiste:
1. Vivado 2020.1
2. Petalinux 2020.1

How to build:
1. Change the directory to fpga/mqnic/250S0C/fpga_pcie/fpga.
2. Run make, this build the hardware design. 
3. Launch Vivado 2020.1 and click on generate bitstream (we will integrate this functionality into the make file in the future).
4. Export the hardware including the bitstream. 
5. Return to the home directory and create a Petalinux project (refer: https://docs.xilinx.com/r/en-US/ug1144-petalinux-tools-reference-guide/PetaLinux-Commands).
6. Clone the repo to the home directory from https://github.com/hemanthr28/flash_util
7. Copy build_binfile.sh from scripts/ folder to the top level of the Petalinux project folder. 
8. Copy build.sh from scripts/ folder to the home directory and run ./build.sh (Update the paths as needed in line 4 and 16)

At this point the Petalinux image with the hardware design will be flashed to the FPGA, power off and power on the host to load the new design. 

The test apps can be found in apps/ folder
