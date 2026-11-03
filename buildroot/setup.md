# Guest image

The guest is stock Buildroot 2026.08 with no changes: `buildroot/defconfig` is `make savedefconfig` output and is identical to `configs/qemu_riscv64_virt_defconfig`. It is kept here to record the versions in use, Linux 6.18.7 and OpenSBI 1.6.

```sh
cd ~
wget https://buildroot.org/downloads/buildroot-2026.08.tar.gz
tar xf buildroot-2026.08.tar.gz
cd buildroot-2026.08
make qemu_riscv64_virt_defconfig
make -j4
```

With 8 GB of RAM, use `-j2`. `scripts/run-qemu.sh` boots the result with the QEMU built from this repo's instructions, not the one Buildroot builds for itself, because that one does not contain the device. Log in as `root`, no password.

The guest reaches the host at `10.0.2.2`. To copy files in, run `python3 -m http.server 8000` in the directory holding them on the host, and `wget http://10.0.2.2:8000/<file>` in the guest.
