ifeq ($(KERNELRELEASE),)
KERNELDIR := ..
PWD := $(shell pwd)
include $(KERNELDIR)/.config
endif

obj-$(CONFIG_POPCORN_KMSG_SOCKET) += msg_socket.o
msg_socket-y := socket.o ring_buffer.o

obj-$(CONFIG_POPCORN_KMSG_TEST) += msg_test.o

.PHONY: modules
modules:
	$(MAKE) -C $(KERNELDIR) M=$(PWD) modules

.PHONY: modules_install
modules_install:
	$(MAKE) -C $(KERNELDIR) M=$(PWD) modules_install

.PHONY: clean
clean:
	$(MAKE) -C $(KERNELDIR) M=$(PWD) clean
