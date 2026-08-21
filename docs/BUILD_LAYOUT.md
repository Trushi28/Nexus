# Build layout

This describes the reformed build structure: renamed Makefiles, a
single canonical copy of the shared kernel<->userland ABI headers, a
`libc/`-style split in userland, and one `build/` directory that every
generated artifact funnels through.

## What changed

1. **`GNUmakefile` -> `Makefile`** at the top level, in `kernel/`, and
   in `userland/`. GNU Make's search order is `GNUmakefile`, then
   `makefile`, then `Makefile` -- renaming loses nothing, `make` still
   just works, and every other tool that expects a file literally
   named `Makefile` (editors, `compile_commands.json` generators, CI
   templates) now finds it without special-casing.

2. **`abi/` is the single source of truth for the syscall ABI.**
   `abi/syscall_nr.h` and `abi/task_info.h` used to be hand-copied
   twice each (`kernel/src/abi/*.h` and `userland/include/*.h`), with
   a comment on each copy begging you to remember to update the other
   one. Now there is exactly one file a human ever edits. `make
   sync-abi` (via `scripts/sync-abi.sh`) regenerates both build
   trees' copies from it, stamped with a "GENERATED FILE" banner;
   both `kernel/Makefile` and `userland/Makefile` run this
   automatically as part of `all`, so you don't need to think about
   it day to day. `make check-abi` (CI-oriented) fails loudly if a
   generated copy was ever hand-edited instead of `abi/` itself.

   Why two copies still exist at all instead of one shared include
   path: the kernel and userland are deliberately separate,
   freestanding build trees with no shared installed sysroot (kernel
   is a PIE with its own linker script and W^X page tables; userland
   is a static ET_EXEC with its own crt0/linker script) -- see
   `docs/DESIGN.md`. Sharing an include path across them would be a
   bigger, riskier change than fixing the actual problem, which was
   never "two build trees" but "two copies with no mechanism keeping
   them honest."

3. **`userland/libc/`** now holds Nexus's own tiny freestanding
   libc-equivalent: `ulib.c`, `ulib.h`, `crt0.S`, `syscall.h` (moved
   out of `userland/` itself, unchanged otherwise). It builds once
   into `libnexus.a`, and every demo program links against the
   archive instead of each program's build rule recompiling
   `ulib.c`/`crt0.S` from scratch. Add new libc functionality under
   `libc/`; add a new demo program by dropping a `.c` file directly in
   `userland/` and adding its name to `PROGRAMS` in
   `userland/Makefile`.

4. **Everything funnels through `build/`.** Object files, linked
   binaries, `libnexus.a`, the fetched `limine.h`/`font8x8_basic.h`,
   the fetched Limine bootloader release and OVMF firmware, `iso_root/`,
   `initrd_root/`, `initrd.tar`, the final `nexus.iso`/`nexus.hdd`, even
   the scratch QEMU test disk -- all of it lands under `build/`.
   `rm -rf build` always gets you back to a byte-for-byte clean
   checkout; nothing this repo's Makefiles produce lives anywhere
   else. Override the location with `make BUILD_DIR=/somewhere/else`.

## Layout

```
Makefile                  top-level orchestration (was GNUmakefile)
abi/                      canonical shared ABI headers -- edit these, nothing else
  syscall_nr.h
  task_info.h
scripts/
  sync-abi.sh              regenerates kernel/src/abi/*.h + userland/include/*.h from abi/
  check-abi.sh              CI guard: fails if a generated copy drifted from abi/
kernel/
  Makefile                 (was GNUmakefile)
  get-deps                 fetches limine.h/font8x8_basic.h into $(BUILD_DIR)/include
  src/abi/                 GENERATED (gitignored) -- do not edit
userland/
  Makefile                 (was GNUmakefile)
  libc/                    Nexus's own libc-equivalent -> build/userland/lib/libnexus.a
    ulib.c ulib.h crt0.S syscall.h
  hello.c sysinfo.c ...    demo programs (unchanged, still live directly in userland/)
  include/                 GENERATED (gitignored) -- do not edit
build/                     GITIGNORED. Everything generated. Delete freely.
  kernel/{obj,bin,include}
  userland/{obj,bin,lib}
  limine-binary/  edk2-ovmf-bins/
  iso_root/  initrd_root/  initrd.tar
  nexus.iso  nexus.hdd  test_disk.img
```

## Common commands

```sh
make              # -> build/nexus.iso
make all-hdd      # -> build/nexus.hdd
make run          # boots build/nexus.iso in QEMU (BIOS)
make kernel       # just the kernel, at build/kernel/bin/kernel
make userland     # just the demo programs, at build/userland/bin/*
make sync-abi     # manually regenerate the two ABI header copies
make check-abi    # CI: fail if a generated copy was hand-edited
make clean        # removes build output, keeps fetched third-party deps
make distclean    # rm -rf build -- fully clean checkout
```

Each subdirectory's Makefile is still independently runnable
(`make -C kernel`, `make -C userland`) and defaults `BUILD_DIR` to its
own `./build` when invoked that way, so you can iterate on just the
kernel or just userland without going through the top-level Makefile
at all.
