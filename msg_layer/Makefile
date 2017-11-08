# To build modules outside of the kernel tree, we run "make"
# in the kernel source tree; the Makefile these then includes this
# Makefile once again.
# This conditional selects whether we are being included from the
# kernel Makefile or not.

ifneq ($(CONFIG_POPCORN_KMSG_DOLPHIN),)
# XXX set installation path accordingly
DOLPHIN = /path/to/source/DIS

EXTRA_CFLAGS += \
	-I$(DOLPHIN)/src/IRM_GX/drv/src \
	-I$(DOLPHIN)/src/IRM_GX/drv/src/LINUX \
	-I$(DOLPHIN)/src/COMMON/osif/kernel/include \
	-I$(DOLPHIN)/src/COMMON/osif/kernel/include/LINUX \
	-I$(DOLPHIN)/src/include/ \
	-I$(DOLPHIN)/src/include/dis/ \
	-I$(DOLPHIN)/include

EXTRA_CFLAGS += -D_KERNEL
EXTRA_CFLAGS += -DOS_IS_LINUX

KBUILD_EXTRA_SYMBOLS = $(DOLPHIN)/src/IRM_GX/drv/src/LINUX/Module.symvers
endif # CONFIG_POPCORN_MSG_DOLPHIN


ifeq ($(KERNELRELEASE),)
    # Assume the source tree is where the running kernel was built
    # You should set KERNELDIR in the environment if it's elsewhere
    KERNELDIR ?= /lib/modules/$(shell uname -r)/build
    # The current directory is passed to sub-makes as argument
    PWD := $(shell pwd)

.PHONY: modules
modules:
	$(MAKE) -C $(KERNELDIR) M=$(PWD) modules

.PHONY: modules_install
modules_install:
	$(MAKE) -C $(KERNELDIR) M=$(PWD) modules_install

.PHONY: clean
clean:
	rm -rf *.o *~ core .depend .*.cmd *.ko *.mod.c .tmp_versions Module.symvers modules.order

else # KERNELRELEASE
    # called from kernel build system
    obj-$(CONFIG_POPCORN_KMSG_LOOPBACK) += msg_loopback.o
    obj-$(CONFIG_POPCORN_KMSG_SOCKET) += msg_socket.o
    obj-$(CONFIG_POPCORN_KMSG_DOLPHIN) += msg_dolphin.o
    obj-$(CONFIG_POPCORN_KMSG_IB) += msg_ib.o
    obj-$(CONFIG_POPCORN_KMSG_TEST) += msg_test.o
endif
