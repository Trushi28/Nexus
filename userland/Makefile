# Nexus userland demo programs.
#
CC := x86_64-elf-gcc
LD := x86_64-elf-ld
AR := x86_64-elf-ar

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

.SECONDARY: $(CRT0_OBJ)

.PHONY: all
all: sync-abi $(addprefix $(BINDIR)/,$(PROGRAMS))

.PHONY: sync-abi
sync-abi:
	@../scripts/sync-abi.sh

$(OBJDIR)/libc/%.S.o: libc/%.S Makefile
	@mkdir -p $(@D)
	$(CC) $(CFLAGS) -c $< -o $@

$(OBJDIR)/libc/%.c.o: libc/%.c libc/ulib.h Makefile
	@mkdir -p $(@D)
	$(CC) $(CFLAGS) -c $< -o $@

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
