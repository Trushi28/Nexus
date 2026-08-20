# Nexus userland demo programs.
#
# Same trick the kernel build already relies on -- your distro's native
# gcc/clang + ld work fine for a freestanding, -nostdlib target, we
# don't need a dedicated cross toolchain. Unlike the kernel (a
# relocatable PIE, per Limine's requirements), these are static,
# non-PIE ET_EXEC binaries: see user.ld and proc/elf.c's loader, which
# only understands that shape.

CC := cc
LD := ld

CFLAGS := -Wall -Wextra -std=gnu11 -O2 \
          -ffreestanding -fno-stack-protector -fno-stack-check \
          -fno-pic -fno-pie -m64 -march=x86-64 \
          -mno-80387 -mno-mmx -mno-sse -mno-sse2 -mno-red-zone \
          -Iinclude -I.

LDFLAGS := -m elf_x86_64 -static -no-pie -T user.ld

PROGRAMS := hello sysinfo guess nsh mkfile splitdemo multisplit

.PHONY: all
all: $(addprefix bin/,$(PROGRAMS))

bin/%: %.c crt0.S ulib.c ulib.h syscall.h include/syscall_nr.h include/task_info.h user.ld
	@mkdir -p bin obj
	$(CC) $(CFLAGS) -c crt0.S -o obj/crt0.o
	$(CC) $(CFLAGS) -c ulib.c -o obj/ulib.o
	$(CC) $(CFLAGS) -c $*.c -o obj/$*.o
	$(LD) $(LDFLAGS) obj/crt0.o obj/ulib.o obj/$*.o -o $@

.PHONY: clean
clean:
	rm -rf bin obj
