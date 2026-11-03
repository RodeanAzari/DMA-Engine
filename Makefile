# Builds the driver and userspace for the running kernel. To cross-build for
# the guest, see the README. The device model builds inside a QEMU tree.
all: driver userspace

driver:
	$(MAKE) -C driver

userspace:
	$(MAKE) -C userspace

regs:
	python3 scripts/gen-regs.py

clean:
	$(MAKE) -C driver clean
	$(MAKE) -C userspace clean

.PHONY: all driver userspace regs clean
