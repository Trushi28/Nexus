# Nexus kernel build rules.

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

override CPPFLAGS := \
    -I $(DEPSDIR) \
    -I src \
    $(CPPFLAGS)

override LDFLAGS += \
    -m elf_x86_64 \
    -static \
    -pie \
    --no-dynamic-linker \
    -z text \
    -z max-page-size=0x1000 \
    -T linker.ld

override CFILES := $(shell find -L src -type f -name '*.c')
override ASFILES := $(shell find -L src -type f -name '*.S')
override OBJ := $(addprefix $(OBJDIR)/,$(CFILES:.c=.c.o) $(ASFILES:.S=.S.o))
override HEADER_DEPS := $(addprefix $(OBJDIR)/,$(CFILES:.c=.c.d) $(ASFILES:.S=.S.d))

.PHONY: all
all: sync-abi $(KERNEL)

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
