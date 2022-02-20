# zephyr_for_pophype
Zephyr base for pophype demonstration

 export OVMF_FD_PATH=/usr/share/OVMF/OVMF_CODE.fd

How to build ARM64
1) west build -b qemu_cortex_a53 /home/ashwin/ep_aarch64/ -DCMAKE_VERBOSE_MAKEFILE=ON
2) copy the config file from application folder and paste it as .config in zephyrproject/build/zephyr/.config

PARSEC build folders ivshmem_arm64 and blackscholes_x86

red_black_tree _ [ARCH] folder actually contains md5 code. 
