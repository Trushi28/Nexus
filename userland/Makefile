# Nexus userland demo programs.
#
# Same trick the kernel build already relies on -- your distro's native
# gcc/clang + ld work fine for a freestanding, -nostdlib target, we
# don't need a dedicated cross toolchain. Unlike the kernel (a
# relocatable PIE, per Limine's requirements), these are static,
# non-PIE ET_EXEC binaries: see user.ld and proc/elf.c's loader, which
# only understands that shape.
#
# libc/ is Nexus's own tiny freestanding libc-equivalent (ulib.*,
# crt0.S, syscall.h) -- built once into libnexus.a and linked into
# every program below, instead of each program's build rule
# recompiling ulib.c/crt0.S from scratch (the old behaviour). Add a
# new demo program by dropping a .c file in this directory (userland/,
# next to this Makefile) and adding its name to $(PROGRAMS); add new
# libc functionality under libc/.
#
# Everything this produces lands under $(BUILD_DIR) -- see
# docs/BUILD_LAYOUT.md. Invoked from the top-level Makefile with
# BUILD_DIR already set to .../build/userland; running `make` directly
# inside userland/ still works standalone (BUILD_DIR defaults to
# userland/build).

CC := cc
LD := ld
AR := ar

override BUILD_DIR := $(abspath $(if $(BUILD_DIR),$(BUILD_DIR),build))
override OBJDIR := $(BUILD_DIR)/obj
override BINDIR := $(BUILD_DIR)/bin
override LIBDIR := $(BUILD_DIR)/lib

CFLAGS := -Wall -Wextra -std=gnu11 -O2 \
          -ffreestanding -fno-stack-protector -fno-stack-check \
          -fno-pic -fno-pie -m64 -march=x86-64 \
          -mno-80387 -mno-mmx -mno-sse -mno-sse2 -mno-red-zone \
          -Ilibc -Iinclude -I.

LDFLAGS := -m elf_x86_64 -static -no-pie -T user.ld

PROGRAMS := hello sysinfo guess nsh mkfile splitdemo multisplit

LIBNEXUS := $(LIBDIR)/libnexus.a
CRT0_OBJ := $(OBJDIR)/libc/crt0.S.o
LIBC_OBJ := $(OBJDIR)/libc/ulib.c.o

# Without this, make treats $(CRT0_OBJ) as a disposable intermediate
# (built via the %.S.o pattern rule below, never named directly by
# `all`) and deletes it after the last program links against it --
# harmless, but it means every build recompiles crt0.S from scratch
# instead of reusing it.
.SECONDARY: $(CRT0_OBJ)

.PHONY: all
all: sync-abi $(addprefix $(BINDIR)/,$(PROGRAMS))

# Regenerates include/syscall_nr.h and include/task_info.h from the
# canonical copy in ../abi/ -- see scripts/sync-abi.sh. Run
# unconditionally on every build, same reasoning as the kernel
# Makefile's own sync-abi target.
.PHONY: sync-abi
sync-abi:
	@../scripts/sync-abi.sh

$(OBJDIR)/libc/%.S.o: libc/%.S Makefile
	@mkdir -p $(@D)
	$(CC) $(CFLAGS) -c $< -o $@

$(OBJDIR)/libc/%.c.o: libc/%.c libc/ulib.h Makefile
	@mkdir -p $(@D)
	$(CC) $(CFLAGS) -c $< -o $@

# libnexus.a is just ulib.o -- crt0.o is linked in separately, per
# program, rather than archived: it defines _start, and pulling a
# definition of _start out of a .a only happens if something already
# undefined references it, which nothing does until link time picks
# the program's own object first. Simpler to just always pass it
# explicitly, like the old per-program rule did.
$(LIBNEXUS): $(LIBC_OBJ)
	@mkdir -p $(@D)
	rm -f $@
	$(AR) rcs $@ $(LIBC_OBJ)

$(BINDIR)/%: %.c $(CRT0_OBJ) $(LIBNEXUS) libc/syscall.h include/syscall_nr.h include/task_info.h include/sysinfo.h user.ld
	@mkdir -p $(OBJDIR) $(BINDIR)
	$(CC) $(CFLAGS) -c $*.c -o $(OBJDIR)/$*.c.o
	$(LD) $(LDFLAGS) $(CRT0_OBJ) $(OBJDIR)/$*.c.o $(LIBNEXUS) -o $@

.PHONY: clean
clean:
	rm -rf $(BUILD_DIR)/bin $(BUILD_DIR)/obj $(BUILD_DIR)/lib

.PHONY: distclean
distclean:
	rm -rf $(BUILD_DIR)
