<div align="center">

```
 _   _                     
| \ | | _____  ___   _ ___ 
|  \| |/ _ \ \/ / | | / __|
| |\  |  __/>  <| |_| \__ \
|_| \_|\___/_/\_\\__,_|___/
```

**A from-scratch x86-64 kernel — modern hardware only, no legacy crutches.**

![License](https://img.shields.io/badge/license-0BSD-blue)
![Language](https://img.shields.io/badge/language-C11-blue)
![Arch](https://img.shields.io/badge/arch-x86--64-lightgrey)
![Bootloader](https://img.shields.io/badge/bootloader-Limine-orange)

</div>

Nexus targets modern x86-64 hardware only — x2APIC, ACPI, SMP via
Limine's MP protocol — with no legacy PIC or BIOS-era fallbacks. Every
core layer (page tables, scheduler, syscalls, ELF loader) is written
from scratch rather than borrowed from the bootloader.

## Features

- **Boot** — Limine protocol, base revision 6, fully modern request set
- **Memory** — bitmap physical allocator, its own W^X page tables (not the bootloader's), cross-CPU TLB shootdown, kernel heap, a slab allocator for fixed-size objects (`struct task`)
- **ACPI / interrupts** — MADT parsing, x2APIC-first LAPIC driver (MMIO xAPIC fallback), I/O APIC routing
- **SMP** — full AP bring-up via Limine's MP feature; tasks migrate freely across cores, mid-syscall included
- **Scheduling** — preemptive round-robin, sleep/wait queues, cooperative context switching
- **Usermode** — ring-3 processes, an ELF64 loader, and a real, deliberately non-POSIX syscall ABI: `split()`/`exec()` instead of fork, plus `read`/`write`/`open`/`close`/`spawn`/`wait`/`wait_any`/`kill`/`ps`/`getuid`/`setuid`/`chdir`/`getcwd`/`brk`/`sysinfo`/`reboot`/`shutdown` and more
- **Filesystem** — GraphFS (a DAG-based filesystem with multi-parent nodes, mark-and-sweep GC, and whole-graph snapshot persistence to disk) as the primary root; `/bin` seeded from a USTAR initrd on every boot
- **Init** — `/bin/init`, a userland service supervisor reading plain service definitions straight out of GraphFS (`/services/<name>/{path,uid,respawn,needs/}`), with launch-order dependency handling and crash-loop protection
- **Drivers** — serial, framebuffer console, PS/2 keyboard, PCI scanner, NVMe (MSI-X), virtio-blk (polled, unverified against real hardware)
- **Shell** — nsh (ring-3, the default interactive shell) plus a ring-0 kernel shell (`kshell` on the cmdline) for privileged/diagnostic commands: `meminfo`, `cpuinfo`, `ps`, `lspci`, `run`, `matrix`, `reboot`, native graph commands (`gsync`/`gload`/`ggc`/`gclear`/...)

## Getting started

**Requirements:** `make`, an `x86_64-elf` cross toolchain (`x86_64-elf-gcc`/`x86_64-elf-ld`/`x86_64-elf-ar`), `curl`, `xorriso`, `sgdisk`, `mtools`. `qemu-system-x86_64` is optional.

```sh
# Arch (AUR):
yay -S x86_64-elf-gcc x86_64-elf-binutils

# macOS (Homebrew):
brew install x86_64-elf-gcc x86_64-elf-binutils

# Anything else: build one yourself -- see the OSDev.org "GCC Cross-
# Compiler" tutorial (https://wiki.osdev.org/GCC_Cross-Compiler).

sudo apt install build-essential curl xorriso gdisk mtools qemu-system-x86

make all-hdd      # -> nexus.hdd (raw disk/USB image)
make all          # -> nexus.iso (BIOS+UEFI hybrid)
```

No cross toolchain? Every native `gcc`/`clang` + `ld` flag this build needs is already explicit (`-ffreestanding`, no implicit CRT, etc. -- see `kernel/Makefile`'s own comment), so falling back to the host toolchain still works fine: `make CC=cc LD=ld` (kernel) / `make CC=cc LD=ld AR=ar` (userland).

Run it:

```sh
make run          # QEMU, BIOS boot
make run-uefi      # QEMU, UEFI boot
```

Flash it to a USB stick:

```sh
sudo dd if=nexus.hdd of=/dev/sdX bs=4M status=progress conv=fsync
```

Double-check `/dev/sdX` before running that.

## Layout

```
kernel/src/    kernel source, one directory per subsystem
               (boot, cpu, mm, acpi, apic, sched, fs, proc, drivers, shell, ...)
userland/      ring-3 demo programs + their own libc/crt0/linker script
limine.conf    bootloader menu
Makefile       top-level build: fetches Limine, builds everything, packs the image
```

## Roadmap

- Graph-native tab completion (sstring names / numeric IDs, not just VFS paths)
- `O_APPEND`
- NVMe: a submit timeout, and PRP chaining past the current ~2MiB single-request cap
- MSI-X for virtio-blk (currently polled-only, and still unverified against real hardware/QEMU)

See [docs/Design.md](docs/Design.md) for design rationale and the full list of known limitations.

## License

0BSD — see [LICENSE](LICENSE).
