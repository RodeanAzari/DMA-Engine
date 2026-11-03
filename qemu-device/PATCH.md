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

## Trace format

`-device dmaeng,trace-file=<path>` appends one record per line, in the order the device saw them. This is the stimulus and expected result for verifying the future RTL. Addresses are guest-physical and vary between runs.

| Record | Meaning |
|---|---|
| `RD <off> <val>` | Register read and the value returned |
| `WR <off> <val>` | Register write |
| `DESC head= addr= src= dst= len= flags= rsvd=` | Descriptor fetched with OWN set, flags as fetched |
| `SRC <hex>` | Source bytes |
| `DST_BEFORE <hex>` | Destination bytes before the copy |
| `DST_AFTER <hex>` | Destination bytes after the copy |
| `DONE status=` | Status written back for that descriptor |
| `MSI` | Interrupt sent (rising edge of `IRQ_STATUS & IRQ_MASK`) |
| `ERROR code=` | Device stopped until `CTRL.RESET` |

A descriptor rejected before the copy has `DESC` and `DONE` but no data records. An error found before a descriptor is owned has `ERROR` only.
