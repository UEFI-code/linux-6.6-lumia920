# 💥 LINUX 6.6 // LUMIA 920 💥

> because a dead mobile platform deserves one last boss fight.

<p align="center">
    <img src="https://github.com/UEFI-code/LumiaToy/blob/main/920_linux.jpg" width="320">
</p>

<p align="center">
    🔥 Windows Phone hardware ✦ Linux kernel ✦ framebuffer sorcery ✦ zero sanity 🔥
</p>

---

## 📼 WHAT IS THIS

This repository is a cursed attempt at booting a modern Linux 6.6 kernel on the Nokia Lumia 920.

The bootloader is unlocked.

The hardware still lives.

So naturally the only reasonable decision was:

"install Linux and start violating memory mappings."

---

## 🧨 BOOTFLOW

UEFI loader:

👉 https://github.com/UEFI-code/LumiaToy

Without it, this repo is just emotionally charged ARM code.

---

## ⚠️ KERNEL HACK ZONE ⚠️

The Lumia 920 has been discontinued for years. Keeping those motherboard pads clean might make you rich in 100 years when some retro-tech collector discovers your untouched hardware shrine (if you are still alive).

This project abuses the UEFI GOP framebuffer as an ultra-early debug console because opening the phone and wiring UART sounds like suffering.

### 🔧 patched areas

- `drivers/firmware/efi/libstub/fdt.c`
    - draws debug color bars during EFI stub stage

- `arch/arm/kernel/head.S`
    - injects `0x80400000` into page tables
    - raw pass-through mapping energy

- `drivers/video/fbdev/lumiafb.c`
    - custom framebuffer driver
    - powered entirely by desperation

- `arch/arm/kernel/setup.c`
    - registers `lumiafb`

- `arch/arm/mm/mmu.c`
    - preserves framebuffer mapping from total annihilation

- `init/main.c`
    - deinit framebuffer before fbcon boots
    - deletes `free_initmem()` because consequences are temporary

---

## 🛠 BUILD

### 1. toolchain setup

```bash
export ARCH=arm
export CROSS_COMPILE=arm-none-eabi-
```

### 2. configure kernel

```bash
make multi_v7_defconfig
make menuconfig
```

Enable these:

```text
UEFI runtime support
Framebuffer Console
Simple framebuffer
Qualcomm MSM support
```

### 3. save config

```bash
cp .config arch/arm/configs/lumia920_defconfig
make lumia920_defconfig
```

### 4. build the monster

```bash
make -j$(nproc)
make qcom/lumia920.dtb # tiny device tree
make qcom/lumia920-simple.dtb # qcom style device tree
```

If the compiler screams, that means it still feels alive.

---

## 🎭 CURRENT STATUS

✅ boots

✅ survived MMU

✅ framebuffer speaks

🔌 GPIO awakened

⚡️ PMIC responds

💾 eMMC remembers

🛸 USB communicates

🔥 Snapdragon S4 has risen from the dead

❓ stability

❓ longevity of this ancient phone

❓ whether this should exist at all

❓ who is actually debugging whom

⚠ ancient Qcom magic remains unexplained

---

## ☣ DISCLAIMER ☣

THIS CODE IS:

- unstable
- unsafe
- experimental
- probably offensive to ARM MMU design principles

Do not trust it with important data.

Do not daily-drive it unless chaos is your primary operating system.

If your Lumia explodes into cosmic dust, that's between you and the framebuffer gods.

---

## 🖤 WHY

because old phones deserve absurd afterlives.

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
