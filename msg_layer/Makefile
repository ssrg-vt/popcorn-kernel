ifeq ($(KERNELRELEASE),)
KERNELDIR := ..
PWD := $(shell pwd)
include $(KERNELDIR)/.config
endif

obj-$(CONFIG_POPCORN_KMSG_SOCKET) += msg_socket.o
obj-$(CONFIG_POPCORN_KMSG_TEST) += msg_test.o
obj-$(CONFIG_POPCORN_KMSG_IB) += msg_ib.o

.PHONY: modules
modules:
	$(MAKE) -C $(KERNELDIR) M=$(PWD) modules
ifdef CONFIG_POPCORN_KMSG_DOLPHIN
	$(MAKE) -C dolphin modules
endif

.PHONY: modules_install
modules_install:
	$(MAKE) -C $(KERNELDIR) M=$(PWD) modules_install
ifdef CONFIG_POPCORN_KMSG_DOLPHIN
	$(MAKE) -C dolphin modules_install
endif

.PHONY: clean
clean:
	$(MAKE) -C $(KERNELDIR) M=$(PWD) clean
	$(MAKE) -C dolphin clean
