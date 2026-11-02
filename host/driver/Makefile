ifneq ($(KERNELRELEASE),)
obj-m := fades.o
else
KDIR ?= /lib/modules/$(shell uname -r)/build
OUT ?= ../../build/host/driver
.PHONY: all clean
all:
	mkdir -p "$(OUT)"
	cp fades.c ../include/fades_ioctl.h Makefile "$(OUT)/"
	$(MAKE) -C "$(KDIR)" M="$(abspath $(OUT))" modules

clean:
	$(MAKE) -C "$(KDIR)" M="$(abspath $(OUT))" clean
endif
