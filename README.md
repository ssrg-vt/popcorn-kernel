fDSM - FPGA-Accelerated DSM framework for Heterogeneous ISA hardware
----------------------------------------------

* FPGA support for Popcorn DeX mainline (https://github.com/ssrg-vt/popcorn-kernel.git). 

* Visit http://popcornlinux.org and https://github.com/ssrg-vt/popcorn-kernel/wiki for more information or e-mail naarayananrao (naarayananrao@vt.edu).

* Copyright Systems Software Research Group at Virginia Tech, 2021-2022.

* Follow the same procedure of installation and loading the driver as given on the Wiki page.

* Currently, fDSM is only supported for a 2-node setup. Manually change the node ID (either 0 or 1) in the FPGA driver (popcorn-kernel/msg_layer/xdma.c - Line # 1004 - my_nid variable). 

* Contact binoy@vt.edu/edsonh@vt.edu/naarayananrao@vt.edu for the FPGA design. Use the .mcs files to program the SPI flash of the FPGA. 



