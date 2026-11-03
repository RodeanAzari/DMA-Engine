#!/bin/sh
# Copy the dmaeng device into a QEMU source tree and register it with the
# build. Safe to run repeatedly. Usage: install-qemu-device.sh [qemu-dir]

set -eu
REPO="$(cd "$(dirname "$0")/.." && pwd)"
QEMU="${1:-$HOME/qemu-11.1.2}"

cp "$REPO/qemu-device/dmaeng.c" "$REPO/include/dmaeng_regs.h" "$QEMU/hw/misc/"

if ! grep -q CONFIG_DMAENG "$QEMU/hw/misc/meson.build"; then
    echo "system_ss.add(when: 'CONFIG_DMAENG', if_true: files('dmaeng.c'))" \
        >> "$QEMU/hw/misc/meson.build"
fi

if ! grep -q "config DMAENG" "$QEMU/hw/misc/Kconfig"; then
    cat >> "$QEMU/hw/misc/Kconfig" <<'EOF'

config DMAENG
    bool
    default y if TEST_DEVICES
    depends on PCI && MSI_NONBROKEN
EOF
fi
