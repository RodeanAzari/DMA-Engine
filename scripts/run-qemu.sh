#!/bin/sh

set -eu
QEMU_PATH=$(which $HOME/qemu-system-riscv64)
IMAGES_PATH=$(which $HOME/output/images)

echo $QEMU_PATH

