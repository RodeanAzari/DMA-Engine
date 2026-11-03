#!/bin/sh

set -eu
QEMU_PATH="${QEMU_PATH:-$HOME/qemu-11.1.2/build/qemu-system-riscv64}"
IMAGES_PATH="${IMAGES_PATH:-$HOME/buildroot-2026.08/output/images}"

exec $QEMU_PATH \
    -M virt,aia=aplic-imsic \
    -bios "$IMAGES_PATH/fw_jump.bin" \
    -append "rootwait root=/dev/vda ro" \
    -kernel "$IMAGES_PATH/Image" \
    -netdev user,id=net0 \
    -device virtio-net-device,netdev=net0 \
    -nographic \
    -drive file="$IMAGES_PATH/rootfs.ext2",format=raw \
    "$@"

    

