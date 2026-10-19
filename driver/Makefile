# Each backend is its own module with the core linked in.
obj-m += dmaeng-host.o
dmaeng-host-y := dmaeng_core.o dmaeng_host.o

ccflags-y += -I$(src)/../include

KDIR ?= /lib/modules/$(shell uname -r)/build

all:
	$(MAKE) -C $(KDIR) M=$(PWD) modules
clean:
	$(MAKE) -C $(KDIR) M=$(PWD) clean
