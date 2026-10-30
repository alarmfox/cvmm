# Build and run

Build the kernel
```sh
make -C <path to kernel src> O=$(pwd)/kbuild-minimal/ KCONFIG_ALLCONFIG=$(pwd)/minimal.config allnoconfig
make -C <path to kernel src> O=$(pwd)/kbuild-minimal/ KCONFIG_ALLCONFIG=$(pwd)/minimal.config -j$(nproc)
```

Build initramfs

```sh
mkdir kbuild-initramfs initramfs
cp busybox.config kbuild-initramfs/.config
make -C <path to busybox src> O=kbuild-initramfs oldconfig
make -C <path to busybox src> O=kbuild-initramfs CONFIG_PREFIX=$(pwd)/initramfs install
mkdir -p initramfs/{dev,proc,sys,tmp}
install -m 755 init $PROJ/initramfs/init
(cd initramfs && find . -print0 | cpio --null -o --format=newc) | gzip -9 > initramfs.cpio.gz
```

Build the vmm:

```sh
make

./kvm_boot kbuild-minimal/arch/x86/boot/bzImage initramfs.cpio.gz
```
