# Nexus kernel build rules.
#
# Modelled on the standard Limine "bare bones" kernel Makefile
# (see https://wiki.osdev.org/Limine_Bare_Bones and the official
# limine-c-template-x86-64 repository), extended with the extra flags a
# kernel that actually enables interrupts and runs on multiple cores needs
# (-mno-red-zone in particular — see the comment below).

.SUFFIXES:

override KERNEL := bin/kernel

# Convenience macro to reliably declare user-overridable variables.
define DEFAULT_VAR =
    ifeq ($(origin $1),default)
        override $(1) := $(2)
    endif
    ifeq ($(origin $1),undefined)
        override $(1) := $(2)
    endif
endef

# It is suggested to use a custom-built cross toolchain to build a kernel,
# but a standard Linux host gcc/clang + binutils targeting x86-64 works
# fine too, which is what this defaults to.
override DEFAULT_CC := cc
$(eval $(call DEFAULT_VAR,CC,$(DEFAULT_CC)))

override DEFAULT_LD := ld
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
override CPPFLAGS := \
    -I include \
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

# Use "find" to glob all *.c and *.S files in the tree (src/ and, once
# get-deps has run, no *.c files under include/ so this stays simple).
override CFILES := $(shell find -L src -type f -name '*.c')
override ASFILES := $(shell find -L src -type f -name '*.S')
override OBJ := $(addprefix obj/,$(CFILES:.c=.c.o) $(ASFILES:.S=.S.o))
override HEADER_DEPS := $(addprefix obj/,$(CFILES:.c=.c.d) $(ASFILES:.S=.S.d))

.PHONY: all
all: $(KERNEL)

$(KERNEL): GNUmakefile linker.ld $(OBJ)
	mkdir -p "$$(dirname $@)"
	$(LD) $(OBJ) $(LDFLAGS) -o $@

-include $(HEADER_DEPS)

obj/%.c.o: %.c GNUmakefile
	mkdir -p "$$(dirname $@)"
	$(CC) $(CFLAGS) $(CPPFLAGS) -c $< -o $@

obj/%.S.o: %.S GNUmakefile
	mkdir -p "$$(dirname $@)"
	$(CC) $(CFLAGS) $(CPPFLAGS) -c $< -o $@

.PHONY: clean
clean:
	rm -rf bin obj

.PHONY: distclean
distclean: clean
	rm -rf include .deps-obtained
