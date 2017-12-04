#
# Makefile for the fpga-cfg driver
#

ifneq ($(KERNELRELEASE),)
	ccflags-y += -I$(TOP_DIR)/include
	obj-m := fpga-cfg.o
else
	KERNELDIR ?= /lib/modules/$(shell uname -r)/build

default:
	$(MAKE) -C $(KERNELDIR) M=$(shell pwd) modules

modules_install:
	$(MAKE) -C $(KERNELDIR) M=$(shell pwd) modules_install

endif

clean:
	-rm -f *.ko* *.mod.* *.o modules.order Modules.symvers
