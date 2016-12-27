# To build modules outside of the kernel tree, we run "make"
# in the kernel source tree; the Makefile these then includes this
# Makefile once again.
# This conditional selects whether we are being included from the
# kernel Makefile or not.

DOLPHIN := /home/vincent/dolphin

EXTRA_CFLAGS := -I$(DOLPHIN)/DIS/src/IRM_GX/drv/src -I$(DOLPHIN)/DIS/src/IRM_GX/drv/src/LINUX -I$(DOLPHIN)/DIS/src/COMMON/osif/kernel/include -I$(DOLPHIN)/DIS/src/COMMON/osif/kernel/include/LINUX -I$(DOLPHIN)/DIS/src/include/ -I$(DOLPHIN)/DIS/src/include/dis/
EXTRA_CFLAGS += -I/opt/DIS/include/ 
EXTRA_CFLAGS += -D_KERNEL -DOS_IS_LINUX

KBUILD_EXTRA_SYMBOLS := $(DOLPHIN)/msg_layer/Module.symvers

ifeq ($(KERNELRELEASE),)

    # Assume the source tree is where the running kernel was built
    # You should set KERNELDIR in the environment if it's elsewhere
    KERNELDIR ?= /lib/modules/$(shell uname -r)/build
    # The current directory is passed to sub-makes as argument
    PWD := $(shell pwd)

modules:
	$(MAKE) -C $(KERNELDIR) M=$(PWD) modules

modules_install:
	$(MAKE) -C $(KERNELDIR) M=$(PWD) modules_install

clean:
	rm -rf *.o *~ core .depend .*.cmd *.ko *.mod.c .tmp_versions Module.symvers modules.order

.PHONY: modules modules_install clean

else
    # called from kernel build system: just declare what our modules are
    obj-m := msg_layer.o
endif
