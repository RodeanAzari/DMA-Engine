# Building the device into QEMU

`qemu-device/dmaeng.c` in this repo is the authority. The copy in the QEMU tree is overwritten by the install script, so never edit it there.

Tested against QEMU 11.1.2.

```sh
scripts/install-qemu-device.sh ~/qemu-11.1.2
cd ~/qemu-11.1.2/build
make -j4
```

The script copies `dmaeng.c` and the generated `dmaeng_regs.h` into `hw/misc/`, and adds the device to `hw/misc/meson.build` and `hw/misc/Kconfig`. It is safe to re-run after any change to the device or the spec.

Check it registered:

```sh
~/qemu-11.1.2/build/qemu-system-riscv64 -device help | grep dmaeng
```

Attach it with `scripts/run-qemu.sh -device dmaeng`, or `-device dmaeng,completion-delay-ns=5000` for delayed completion.
