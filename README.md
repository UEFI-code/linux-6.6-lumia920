# Linux 6.6 for Lumia 920

Since Lumia 920's bootloader has already been unlocked, why not run Linux on it?

<img src="https://github.com/UEFI-code/LumiaToy/blob/main/920_linux.jpg" width="300">

## Loader

Chk `https://github.com/UEFI-code/LumiaToy`

## Kernel Hacks

I don't wana to soldering serial port on the phone motherboard, so I use UEFI GOP's framebuffer as early console

So I need hack:

- `drivers/firmware/efi/libstub/fdt.c`: Draw dbg colorbar during EFI stub
- `arch/arm/kernel/head.S`: Insert 0x80400000 to page table, pass-through
- `drivers/video/fbdev/lumiafb.c`: The drawer
- `arch/arm/kernel/setup.c`: Register lumiafb
- `arch/arm/mm/mmu.c`: Kept 0x80400000 in page table, pass-through
- `init/main.c`: 
    - Deinit lumiafb before fbcon init
    - Remove free_initmem since the page table is hacked

## Build

```bash
export ARCH=arm
export CROSS_COMPILE=arm-none-eabi-
make multi_v7_defconfig
make menuconfig
```

Select

```
UEFI runtime support
Framebuffer Console
Simple framebuffer
Qualcomm MSM support
```

```bash
cp .config arch/arm/configs/lumia920_defconfig
make lumia920_defconfig
```

```
make -j$(nproc)
make qcom/lumia920.dtb
```

## Disclaimer

This code is UNSTABLE and UNSAFE.

Educational purpose only.

DONT DO ANYTHING STUPID.

Use at your own risk.

## Original README

============

There are several guides for kernel developers and users. These guides can
be rendered in a number of formats, like HTML and PDF. Please read
Documentation/admin-guide/README.rst first.

In order to build the documentation, use ``make htmldocs`` or
``make pdfdocs``.  The formatted documentation can also be read online at:

    https://www.kernel.org/doc/html/latest/

There are various text files in the Documentation/ subdirectory,
several of them using the Restructured Text markup notation.

Please read the Documentation/process/changes.rst file, as it contains the
requirements for building and running the kernel, and information about
the problems which may result by upgrading your kernel.
