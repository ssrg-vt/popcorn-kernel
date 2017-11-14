# To build modules outside of the kernel tree, we run "make"
# in the kernel source tree; the Makefile these then includes this
# Makefile once again.
# This conditional selects whether we are being included from the
# kernel Makefile or not.

# XXX set installation path accordingly
DOLPHIN = /home/beowulf/dolphin/DIS

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


ifeq ($(KERNELRELEASE),)
KERNELDIR ?= ../..
PWD := $(shell pwd)

.PHONY: modules
modules:
	$(MAKE) -C $(KERNELDIR) M=$(PWD) modules

.PHONY: modules_install
modules_install:
	$(MAKE) -C $(KERNELDIR) M=$(PWD) modules_install

.PHONY: clean
clean:
	$(MAKE) -C $(KERNELDIR) M=$(PWD) clean

else
obj-$(CONFIG_POPCORN_KMSG_DOLPHIN) += msg_dolphin.o
endif
