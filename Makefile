# Nexus -- top-level build orchestration.
#
# This file is deliberately modelled on the official Limine C template
# (https://github.com/limine-bootloader/limine-c-template-x86-64) so that
# it keeps working as Limine itself evolves. It does three things:
#
#   1. Downloads a pinned Limine binary release (host tool + bootloader
#      images) -- no toolchain other than 'make' and 'curl' needed for this.
#   2. Builds the kernel and userland by recursing into kernel/Makefile and
#      userland/Makefile.
#   3. Packs kernel + Limine into a bootable ISO (BIOS+UEFI) and/or a raw
#      GPT hard-disk/USB image (BIOS+UEFI) that can be `dd`'d to a stick.
#
# EVERYTHING this produces -- kernel/userland object files and binaries,
# the fetched Limine/OVMF/limine.h/font8x8 third-party bits, iso_root/,
# initrd.tar, the final .iso/.hdd images, even the QEMU scratch disk --
# lands under $(BUILD_DIR) (./build by default). `rm -rf build` always
# gets you back to a byte-identical clean checkout; nothing this
# Makefile touches lives outside it. See docs/BUILD_LAYOUT.md.
#
# Nothing here is executed automatically -- this repository ships as source
# only. Run `make all-hdd` (or `make all`) yourself when you're ready.

.SUFFIXES:

# Pin a known-good Limine release. Bump this if you want a newer bootloader.
LIMINE_VERSION := v12.5.2

QEMUFLAGS := -m 2G -smp 4 -cpu qemu64,+x2apic  

override IMAGE_NAME := nexus

HOST_CC := cc
HOST_CFLAGS := -g -O2 -pipe
HOST_CPPFLAGS :=
HOST_LDFLAGS :=
HOST_LIBS :=

# Every build artifact funnels through here -- see the header comment.
# Override on the command line (`make BUILD_DIR=/tmp/nexus-build`) if
# you want it somewhere else; everything below derives from this one
# variable, there's no second place to edit.
override BUILD_DIR := $(abspath $(if $(BUILD_DIR),$(BUILD_DIR),build))

KERNEL_BIN := $(BUILD_DIR)/kernel/bin/kernel
INITRD_TAR := $(BUILD_DIR)/initrd.tar
ISO_IMAGE := $(BUILD_DIR)/$(IMAGE_NAME).iso
HDD_IMAGE := $(BUILD_DIR)/$(IMAGE_NAME).hdd
LIMINE_DIR := $(BUILD_DIR)/limine-binary
OVMF_DIR := $(BUILD_DIR)/edk2-ovmf-bins
ISO_ROOT := $(BUILD_DIR)/iso_root
INITRD_ROOT := $(BUILD_DIR)/initrd_root
TEST_DISK_IMG := $(BUILD_DIR)/test_disk.img

.PHONY: all
all: $(ISO_IMAGE)

.PHONY: all-hdd
all-hdd: $(HDD_IMAGE)

# ---------------------------------------------------------------------------
# Shared ABI headers (kernel/src/abi/, userland/include/) -- both are
# generated from the single canonical copy in abi/. See
# scripts/sync-abi.sh; kernel/Makefile and userland/Makefile also run
# this themselves as part of `kernel`/`userland` below, so you don't
# normally need to invoke it directly.
# ---------------------------------------------------------------------------

.PHONY: sync-abi
sync-abi:
	./scripts/sync-abi.sh

.PHONY: check-abi
check-abi:
	./scripts/check-abi.sh

# ---------------------------------------------------------------------------
# QEMU convenience targets (handy for a first smoke test before real
# hardware -- entirely optional, bare metal does not need QEMU at all).
# ---------------------------------------------------------------------------

.PHONY: run
run: $(ISO_IMAGE)
	qemu-system-x86_64 \
		-M q35 \
		-cdrom $(ISO_IMAGE) \
		-boot d \
		$(QEMUFLAGS)

.PHONY: run-uefi
run-uefi: $(OVMF_DIR) $(ISO_IMAGE)
	qemu-system-x86_64 \
		-M q35 \
		-drive if=pflash,unit=0,format=raw,file=$(OVMF_DIR)/ovmf-code-x86_64.fd,readonly=on \
		-cdrom $(ISO_IMAGE) \
		-boot d \
		$(QEMUFLAGS)

.PHONY: run-hdd
run-hdd: $(HDD_IMAGE)
	qemu-system-x86_64 \
		-M q35 \
		-hda $(HDD_IMAGE) \
		$(QEMUFLAGS)

.PHONY: run-hdd-uefi
run-hdd-uefi: $(OVMF_DIR) $(HDD_IMAGE)
	qemu-system-x86_64 \
		-M q35 \
		-drive if=pflash,unit=0,format=raw,file=$(OVMF_DIR)/ovmf-code-x86_64.fd,readonly=on \
		-hda $(HDD_IMAGE) \
		$(QEMUFLAGS)

$(TEST_DISK_IMG):
	mkdir -p $(BUILD_DIR)
	qemu-img create -f raw $(TEST_DISK_IMG) 64M

.PHONY: run-nvme
run-nvme: $(ISO_IMAGE) $(TEST_DISK_IMG)
	qemu-system-x86_64 \
		-M q35 \
		-cdrom $(ISO_IMAGE) \
		-boot d \
		-drive file=$(TEST_DISK_IMG),if=none,id=nvmedrive,format=raw \
		-device nvme,drive=nvmedrive,serial=deadbeef \
		$(QEMUFLAGS)

.PHONY: run-nvme-uefi
run-nvme-uefi: $(OVMF_DIR) $(ISO_IMAGE) $(TEST_DISK_IMG)
	qemu-system-x86_64 \
		-M q35 \
		-drive if=pflash,unit=0,format=raw,file=$(OVMF_DIR)/ovmf-code-x86_64.fd,readonly=on \
		-cdrom $(ISO_IMAGE) \
		-boot d \
		-drive file=$(TEST_DISK_IMG),if=none,id=nvmedrive,format=raw \
		-device nvme,drive=nvmedrive,serial=deadbeef \
		$(QEMUFLAGS)

# virtio-blk targets deliberately don't also attach an nvme device: the
# boot sequence (main.c's blockdev_init_task) tries nvme_init() first,
# so a machine with both would always end up testing NVMe, not this.
.PHONY: run-virtio-blk
run-virtio-blk: $(ISO_IMAGE) $(TEST_DISK_IMG)
	qemu-system-x86_64 \
		-M q35 \
		-cdrom $(ISO_IMAGE) \
		-boot d \
		-drive file=$(TEST_DISK_IMG),if=none,id=vblkdrive,format=raw \
		-device virtio-blk-pci,drive=vblkdrive \
		$(QEMUFLAGS)

.PHONY: run-virtio-blk-uefi
run-virtio-blk-uefi: $(OVMF_DIR) $(ISO_IMAGE) $(TEST_DISK_IMG)
	qemu-system-x86_64 \
		-M q35 \
		-drive if=pflash,unit=0,format=raw,file=$(OVMF_DIR)/ovmf-code-x86_64.fd,readonly=on \
		-cdrom $(ISO_IMAGE) \
		-boot d \
		-drive file=$(TEST_DISK_IMG),if=none,id=vblkdrive,format=raw \
		-device virtio-blk-pci,drive=vblkdrive \
		$(QEMUFLAGS)

$(OVMF_DIR):
	rm -rf $(OVMF_DIR)
	mkdir -p $(OVMF_DIR)
	cd $(OVMF_DIR) && curl -L https://github.com/osdev0/edk2-ovmf-stable-bins/releases/latest/download/edk2-ovmf-bins.tar.gz | gunzip | tar -xf - --strip-components=1

# ---------------------------------------------------------------------------
# Limine bootloader (host utility + prebuilt stage images)
# ---------------------------------------------------------------------------

$(LIMINE_DIR)/limine:
	mkdir -p $(BUILD_DIR)
	rm -rf $(LIMINE_DIR)
	mkdir -p $(LIMINE_DIR)
	cd $(LIMINE_DIR) && curl -L https://github.com/Limine-Bootloader/Limine/releases/download/$(LIMINE_VERSION)/limine-binary.tar.gz | gunzip | tar -xf - --strip-components=1
	$(MAKE) -C $(LIMINE_DIR) \
		CC="$(HOST_CC)" \
		CFLAGS="$(HOST_CFLAGS)" \
		CPPFLAGS="$(HOST_CPPFLAGS)" \
		LDFLAGS="$(HOST_LDFLAGS)" \
		LIBS="$(HOST_LIBS)"

# ---------------------------------------------------------------------------
# Kernel
# ---------------------------------------------------------------------------

.PHONY: kernel
kernel:
	$(MAKE) -C kernel BUILD_DIR=$(BUILD_DIR)/kernel

# ---------------------------------------------------------------------------
# Userland demo programs + the initrd that ships them to the kernel
# ---------------------------------------------------------------------------

.PHONY: userland
userland:
	$(MAKE) -C userland BUILD_DIR=$(BUILD_DIR)/userland

INITRD_PROGRAMS := hello sysinfo guess nsh mkfile splitdemo multisplit

# A plain ustar archive of every built userland program, under /bin --
# unpacked into tmpfs at boot by fs/initrd.c. `--format=ustar` is
# load-bearing: GNU tar's *default* format ("gnu") isn't what the
# kernel's parser expects.
$(INITRD_TAR): userland
	rm -rf $(INITRD_ROOT)
	mkdir -p $(INITRD_ROOT)/bin
	cp -v $(addprefix $(BUILD_DIR)/userland/bin/,$(INITRD_PROGRAMS)) $(INITRD_ROOT)/bin/
	tar --format=ustar -C $(INITRD_ROOT) -cf $(INITRD_TAR) bin
	rm -rf $(INITRD_ROOT)

# ---------------------------------------------------------------------------
# ISO image (bootable on both legacy BIOS and UEFI)
# ---------------------------------------------------------------------------

$(ISO_IMAGE): $(LIMINE_DIR)/limine kernel $(INITRD_TAR)
	rm -rf $(ISO_ROOT)
	mkdir -p $(ISO_ROOT)/boot
	cp -v $(KERNEL_BIN) $(ISO_ROOT)/boot/
	cp -v $(INITRD_TAR) $(ISO_ROOT)/boot/
	mkdir -p $(ISO_ROOT)/boot/limine
	cp -v limine.conf $(LIMINE_DIR)/limine-bios.sys $(LIMINE_DIR)/limine-bios-cd.bin $(LIMINE_DIR)/limine-uefi-cd.bin $(ISO_ROOT)/boot/limine/
	mkdir -p $(ISO_ROOT)/EFI/BOOT
	cp -v $(LIMINE_DIR)/BOOTX64.EFI $(ISO_ROOT)/EFI/BOOT/
	cp -v $(LIMINE_DIR)/BOOTIA32.EFI $(ISO_ROOT)/EFI/BOOT/
	xorriso -as mkisofs -R -r -J -b boot/limine/limine-bios-cd.bin \
		-no-emul-boot -boot-load-size 4 -boot-info-table -hfsplus \
		-apm-block-size 2048 --efi-boot boot/limine/limine-uefi-cd.bin \
		-efi-boot-part --efi-boot-image --protective-msdos-label \
		$(ISO_ROOT) -o $(ISO_IMAGE)
	$(LIMINE_DIR)/limine bios-install $(ISO_IMAGE)
	rm -rf $(ISO_ROOT)

# ---------------------------------------------------------------------------
# Raw GPT disk/USB image (bootable on both legacy BIOS and UEFI).
# This is the one you `dd` straight to a USB stick for real hardware.
# ---------------------------------------------------------------------------

$(HDD_IMAGE): $(LIMINE_DIR)/limine kernel $(INITRD_TAR)
	rm -f $(HDD_IMAGE)
	dd if=/dev/zero bs=1M count=0 seek=64 of=$(HDD_IMAGE)
	PATH=$$PATH:/usr/sbin:/sbin sgdisk $(HDD_IMAGE) -n 1:2048 -t 1:ef00 -m 1
	$(LIMINE_DIR)/limine bios-install $(HDD_IMAGE)
	mformat -i $(HDD_IMAGE)@@1M
	mmd -i $(HDD_IMAGE)@@1M ::/EFI ::/EFI/BOOT ::/boot ::/boot/limine
	mcopy -i $(HDD_IMAGE)@@1M $(KERNEL_BIN) ::/boot
	mcopy -i $(HDD_IMAGE)@@1M $(INITRD_TAR) ::/boot
	mcopy -i $(HDD_IMAGE)@@1M limine.conf $(LIMINE_DIR)/limine-bios.sys ::/boot/limine
	mcopy -i $(HDD_IMAGE)@@1M $(LIMINE_DIR)/BOOTX64.EFI ::/EFI/BOOT
	mcopy -i $(HDD_IMAGE)@@1M $(LIMINE_DIR)/BOOTIA32.EFI ::/EFI/BOOT

.PHONY: clean
clean:
	$(MAKE) -C kernel BUILD_DIR=$(BUILD_DIR)/kernel clean
	$(MAKE) -C userland BUILD_DIR=$(BUILD_DIR)/userland clean
	rm -rf $(ISO_ROOT) $(INITRD_ROOT) $(INITRD_TAR) $(ISO_IMAGE) $(HDD_IMAGE)

.PHONY: distclean
distclean:
	rm -rf $(BUILD_DIR)
