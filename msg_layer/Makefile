PWD = $(shell pwd)

.PHONY: modules
modules:
	$(MAKE) -C .. M=$(PWD) modules
	$(MAKE) -C dolphin modules

.PHONY: modules_install
modules_install:
	$(MAKE) -C .. M=$(PWD) modules_install
	$(MAKE) -C dolphin modules_install

.PHONY: clean
clean:
	$(MAKE) -C .. M=$(PWD) clean
	$(MAKE) -C dolphin clean
