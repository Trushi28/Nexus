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

## Screenshots

*Coming soon.*

<!--
<p align="center">
  <img src="docs/screenshots/boot.png" width="800" alt="Boot sequence">
  <img src="docs/screenshots/shell.png" width="800" alt="Interactive shell">
</p>
-->

## Features

- **Boot** — Limine protocol, base revision 6, fully modern request set
- **Memory** — bitmap physical allocator, its own W^X page tables (not the bootloader's), cross-CPU TLB shootdown, kernel heap
- **ACPI / interrupts** — MADT parsing, x2APIC-first LAPIC driver (MMIO xAPIC fallback), I/O APIC routing
- **SMP** — full AP bring-up via Limine's MP feature; tasks migrate freely across cores, mid-syscall included
- **Scheduling** — preemptive round-robin, sleep/wait queues, cooperative context switching
- **Usermode** — ring-3 processes, ELF64 loader, a real syscall ABI (`exit`, `read`, `write`, `open`, `close`, `getpid`, `sleep_ms`, `yield`, `brk`, `readdir`, `uptime_ms`)
- **Filesystem** — small VFS, primary root backed by GraphFS (a graph-based filesystem, persisted to disk), `/bin` seeded from a USTAR initrd
- **Drivers** — serial, framebuffer console, PS/2 keyboard, PCI scanner, NVMe, virtio-blk
- **Shell** — nsh (ring-3, the default interactive shell) plus a ring-0 kernel shell (`kshell` on the cmdline) for privileged/diagnostic commands: `meminfo`, `cpuinfo`, `ps`, `lspci`, `ls`, `cat`, `run`, `matrix`, `reboot`, `loom`, native graph commands (`gsync`/`gload`/`ggc`/`gclear`/...)

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
GNUmakefile    top-level build: fetches Limine, builds everything, packs the image
```

## Roadmap

- `fork` / `exec`

Already shipped, despite older notes to the contrary: real `copy_from_user`/
`copy_to_user` with page-fault recovery (`cpu/usercopy.c`), background jobs
(`run <path> &`, `jobs`, `kill`) in both shells, an MSI-X-driven (not
polled) NVMe driver, `O_RDONLY`/`O_WRONLY` enforcement on open file
descriptors, a generic VFS mount table (`vfs_mount()`/`vfs_unmount()` --
currently unused, ready for whenever a second real mountable filesystem
shows up), a `struct slab_cache` allocator (`mm/slab.c`) now backing
`struct task` allocation instead of the general heap, and -- as of this
pass -- **GraphFS as the actual primary filesystem** (not a secondary
fallback grafted onto a tmpfs root): `/bin` now lives in the graph
itself, persists across reboots via `gsync`/`gload` like everything
else in it, and nsh (`/bin/nsh`) is the default interactive shell,
with the ring-0 kernel shell reachable via `kshell` on the cmdline --
see `docs/DESIGN.md`'s Known limitations section and Kernel command
line section for the details.
A second block device driver, virtio-blk (`drivers/virtio_blk.c`),
also exists behind the same `drivers/blockdev.h` HAL as NVMe -- but
it's compile-tested only, **not yet verified against real hardware or
QEMU** (see the header comment); treat it as unverified until someone
actually boots `make run-virtio-blk`.

For the design rationale behind specific choices and how this has been
tested, see [docs/DESIGN.md](docs/DESIGN.md).

## License

0BSD — see [LICENSE](LICENSE).
