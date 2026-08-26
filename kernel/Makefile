# Nexus kernel build rules.
#
# Modelled on the standard Limine "bare bones" kernel Makefile
# (see https://wiki.osdev.org/Limine_Bare_Bones and the official
# limine-c-template-x86-64 repository), extended with the extra flags a
# kernel that actually enables interrupts and runs on multiple cores needs
# (-mno-red-zone in particular -- see the comment below).
#
# Everything this produces (object files, the linked kernel binary, and
# the two headers get-deps fetches) lands under $(BUILD_DIR), never
# inside this source tree -- see docs/BUILD_LAYOUT.md. Invoked from the
# top-level Makefile with BUILD_DIR already set to .../build/kernel;
# running `make` directly inside kernel/ still works standalone and
# just defaults BUILD_DIR to kernel/build.

.SUFFIXES:

override BUILD_DIR := $(abspath $(if $(BUILD_DIR),$(BUILD_DIR),build))

override KERNEL := $(BUILD_DIR)/bin/kernel
override OBJDIR := $(BUILD_DIR)/obj
override DEPSDIR := $(BUILD_DIR)/include

# Convenience macro to reliably declare user-overridable variables.
define DEFAULT_VAR =
    ifeq ($(origin $1),default)
        override $(1) := $(2)
    endif
    ifeq ($(origin $1),undefined)
        override $(1) := $(2)
    endif
endef

# Defaults to a real x86_64-elf cross toolchain (e.g. Arch's AUR
# `x86_64-elf-gcc` package, or the OSDev.org "GCC Cross-Compiler"
# tutorial's own build) rather than the host's native gcc/clang +
# binutils. Both have always worked here -- every freestanding-build
# flag this kernel needs (-ffreestanding, -fno-stack-protector, no
# implicit CRT/libc, etc.) is already explicit below rather than
# assumed from a hosted-target default -- but a genuine x86_64-elf
# target has no hosted-target assumptions to override in the first
# place (no default PIE-by-distro, no implicit stack-protector-strong,
# no host-triple-specific quirks), which is the textbook-correct way
# to build a freestanding kernel. Still fully override-able, e.g.
# `make CC=cc LD=ld` to fall back to the host toolchain if a cross
# compiler isn't installed.
override DEFAULT_CC := x86_64-elf-gcc
$(eval $(call DEFAULT_VAR,CC,$(DEFAULT_CC)))

override DEFAULT_LD := x86_64-elf-ld
$(eval $(call DEFAULT_VAR,LD,$(DEFAULT_LD)))

# User-controllable C flags.
override DEFAULT_CFLAGS := -g -O2 -pipe

# User-controllable C preprocessor flags. We set none by default.
override DEFAULT_CPPFLAGS :=

# User-controllable linker flags. We set none by default.
override DEFAULT_LDFLAGS :=

$(eval $(call DEFAULT_VAR,CFLAGS,$(DEFAULT_CFLAGS)))
$(eval $(call DEFAULT_VAR,CPPFLAGS,$(DEFAULT_CPPFLAGS)))
$(eval $(call DEFAULT_VAR,LDFLAGS,$(DEFAULT_LDFLAGS)))

# Internal C flags that should not be changed by the user.
override CFLAGS += \
    -Wall \
    -Wextra \
    -std=gnu11 \
    -ffreestanding \
    -fno-stack-protector \
    -fno-stack-check \
    -fno-lto \
    -fPIE \
    -m64 \
    -march=x86-64 \
    -mno-80387 \
    -mno-mmx \
    -mno-sse \
    -mno-sse2 \
    -mno-red-zone \
    -MMD

# -mno-red-zone deserves a note: this kernel enables interrupts (APIC
# timer, keyboard, IPIs...) almost immediately after boot. The SysV red
# zone (128 bytes below RSP a leaf function may scribble on without
# adjusting RSP) is NOT safe once an interrupt can land on the current
# stack, because the CPU pushes the interrupt frame right where that data
# lives. Every C file must be built without it, or you'll get infrequent,
# maddening stack corruption that only reproduces under load.

# Internal C preprocessor flags that should not be changed by the user.
# $(DEPSDIR) holds limine.h/font8x8_basic.h, fetched by get-deps into
# the build directory -- see that script and $(BUILD_DIR)/.deps-obtained
# below.
override CPPFLAGS := \
    -I $(DEPSDIR) \
    -I src \
    $(CPPFLAGS)

# Internal linker flags that should not be changed by the user.
# These are passed to `ld` directly (not through the `cc` driver) --
# building a freestanding kernel needs no CRT startup files or implicit
# libraries, so there is nothing the gcc driver needs to add for us, and
# -nostdlib (a gcc-driver-only flag with no ld equivalent) is therefore
# neither needed nor valid here.
override LDFLAGS += \
    -m elf_x86_64 \
    -static \
    -pie \
    --no-dynamic-linker \
    -z text \
    -z max-page-size=0x1000 \
    -T linker.ld

# Use "find" to glob all *.c and *.S files in the tree (src/ only --
# fetched deps now live under $(DEPSDIR), outside src/, so there's
# nothing to accidentally sweep up there any more).
override CFILES := $(shell find -L src -type f -name '*.c')
override ASFILES := $(shell find -L src -type f -name '*.S')
override OBJ := $(addprefix $(OBJDIR)/,$(CFILES:.c=.c.o) $(ASFILES:.S=.S.o))
override HEADER_DEPS := $(addprefix $(OBJDIR)/,$(CFILES:.c=.c.d) $(ASFILES:.S=.S.d))

.PHONY: all
all: sync-abi $(KERNEL)

# Regenerates kernel/src/abi/*.h from the canonical copy in ../abi/ --
# see scripts/sync-abi.sh. Cheap (a handful of small file copies), run
# unconditionally on every build rather than tracked as a file
# dependency, so an edit to ../abi/*.h is never missed.
.PHONY: sync-abi
sync-abi:
	@../scripts/sync-abi.sh

$(BUILD_DIR)/.deps-obtained:
	BUILD_DIR=$(BUILD_DIR) ./get-deps

$(KERNEL): Makefile linker.ld $(BUILD_DIR)/.deps-obtained $(OBJ)
	mkdir -p "$$(dirname $@)"
	$(LD) $(OBJ) $(LDFLAGS) -o $@

-include $(HEADER_DEPS)

$(OBJDIR)/%.c.o: %.c Makefile $(BUILD_DIR)/.deps-obtained
	mkdir -p "$$(dirname $@)"
	$(CC) $(CFLAGS) $(CPPFLAGS) -c $< -o $@

$(OBJDIR)/%.S.o: %.S Makefile $(BUILD_DIR)/.deps-obtained
	mkdir -p "$$(dirname $@)"
	$(CC) $(CFLAGS) $(CPPFLAGS) -c $< -o $@

.PHONY: clean
clean:
	rm -rf $(BUILD_DIR)/bin $(BUILD_DIR)/obj

.PHONY: distclean
distclean:
	rm -rf $(BUILD_DIR)
