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
- **Filesystem** — small VFS + tmpfs, booted from a USTAR initrd
- **Drivers** — serial, framebuffer console, PS/2 keyboard, PCI scanner
- **Shell** — `meminfo`, `cpuinfo`, `ps`, `lspci`, `ls`, `cat`, `run`, `matrix`, `reboot`

## Getting started

**Requirements:** `make`, a native `gcc`/`clang` + `ld` (no cross-compiler needed), `curl`, `xorriso`, `sgdisk`, `mtools`. `qemu-system-x86_64` is optional.

```sh
sudo apt install build-essential curl xorriso gdisk mtools qemu-system-x86

make all-hdd      # -> nexus.hdd (raw disk/USB image)
make all          # -> nexus.iso (BIOS+UEFI hybrid)
```

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
- Slab allocator
- Block device driver (virtio-blk)

Already shipped, despite older notes to the contrary: real `copy_from_user`/
`copy_to_user` with page-fault recovery (`cpu/usercopy.c`), background jobs
(`run <path> &`, `jobs`, `kill`) in both shells, an MSI-X-driven (not
polled) NVMe driver, `O_RDONLY`/`O_WRONLY` enforcement on open file
descriptors, a generic VFS mount table (`vfs_mount()`/`vfs_unmount()` --
currently unused, since the graph filesystem uses the separate, simpler
`vfs_set_root_fallback()` instead; ready for whenever a second real
mountable filesystem shows up), and the graph filesystem grafted
transparently into the classic path namespace (`sstring photos <node>`
makes `/photos` work like any other top-level path -- see
`fs/graphfs_vfs.c`).

For the design rationale behind specific choices and how this has been
tested, see [docs/DESIGN.md](docs/DESIGN.md).

## License

0BSD — see [LICENSE](LICENSE).
