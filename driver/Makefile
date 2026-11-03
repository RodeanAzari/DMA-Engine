# Each backend is its own module with the core linked in.
obj-m += dmaeng-host.o
dmaeng-host-y := dmaeng_core.o dmaeng_host.o
obj-m += dmaeng-pci.o
dmaeng-pci-y := dmaeng_core.o dmaeng_pci.o

ccflags-y += -I$(src)/../include
ifdef FAULT_INJECT
ccflags-y += -DDMAENG_FAULT_INJECT
endif

KDIR ?= /lib/modules/$(shell uname -r)/build

all:
	$(MAKE) -C $(KDIR) M=$(CURDIR) modules
clean:
	$(MAKE) -C $(KDIR) M=$(CURDIR) clean
