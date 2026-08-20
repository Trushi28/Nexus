# Nexus — top-level build orchestration.
#
# This file is deliberately modelled on the official Limine C template
# (https://github.com/limine-bootloader/limine-c-template-x86-64) so that
# it keeps working as Limine itself evolves. It does three things:
#
#   1. Downloads a pinned Limine binary release (host tool + bootloader
#      images) — no toolchain other than 'make' and 'curl' needed for this.
#   2. Builds the kernel by recursing into kernel/GNUmakefile.
#   3. Packs kernel + Limine into a bootable ISO (BIOS+UEFI) and/or a raw
#      GPT hard-disk/USB image (BIOS+UEFI) that can be `dd`'d to a stick.
#
# Nothing here is executed automatically — this repository ships as source
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

.PHONY: all
all: $(IMAGE_NAME).iso

.PHONY: all-hdd
all-hdd: $(IMAGE_NAME).hdd

# ---------------------------------------------------------------------------
# QEMU convenience targets (handy for a first smoke test before real
# hardware — entirely optional, bare metal does not need QEMU at all).
# ---------------------------------------------------------------------------

.PHONY: run
run: $(IMAGE_NAME).iso
	qemu-system-x86_64 \
		-M q35 \
		-cdrom $(IMAGE_NAME).iso \
		-boot d \
		$(QEMUFLAGS)

.PHONY: run-uefi
run-uefi: edk2-ovmf-bins $(IMAGE_NAME).iso
	qemu-system-x86_64 \
		-M q35 \
		-drive if=pflash,unit=0,format=raw,file=edk2-ovmf-bins/ovmf-code-x86_64.fd,readonly=on \
		-cdrom $(IMAGE_NAME).iso \
		-boot d \
		$(QEMUFLAGS)

.PHONY: run-hdd
run-hdd: $(IMAGE_NAME).hdd
	qemu-system-x86_64 \
		-M q35 \
		-hda $(IMAGE_NAME).hdd \
		$(QEMUFLAGS)

.PHONY: run-hdd-uefi
run-hdd-uefi: edk2-ovmf-bins $(IMAGE_NAME).hdd
	qemu-system-x86_64 \
		-M q35 \
		-drive if=pflash,unit=0,format=raw,file=edk2-ovmf-bins/ovmf-code-x86_64.fd,readonly=on \
		-hda $(IMAGE_NAME).hdd \
		$(QEMUFLAGS)

TEST_DISK_IMG := test_disk.img

$(TEST_DISK_IMG):
	qemu-img create -f raw $(TEST_DISK_IMG) 64M

.PHONY: run-nvme
run-nvme: $(IMAGE_NAME).iso $(TEST_DISK_IMG)
	qemu-system-x86_64 \
		-M q35 \
		-cdrom $(IMAGE_NAME).iso \
		-boot d \
		-drive file=$(TEST_DISK_IMG),if=none,id=nvmedrive,format=raw \
		-device nvme,drive=nvmedrive,serial=deadbeef \
		$(QEMUFLAGS)

.PHONY: run-nvme-uefi
run-nvme-uefi: edk2-ovmf-bins $(IMAGE_NAME).iso $(TEST_DISK_IMG)
	qemu-system-x86_64 \
		-M q35 \
		-drive if=pflash,unit=0,format=raw,file=edk2-ovmf-bins/ovmf-code-x86_64.fd,readonly=on \
		-cdrom $(IMAGE_NAME).iso \
		-boot d \
		-drive file=$(TEST_DISK_IMG),if=none,id=nvmedrive,format=raw \
		-device nvme,drive=nvmedrive,serial=deadbeef \
		$(QEMUFLAGS)

# virtio-blk targets deliberately don't also attach an nvme device: the
# boot sequence (main.c's blockdev_init_task) tries nvme_init() first,
# so a machine with both would always end up testing NVMe, not this.
.PHONY: run-virtio-blk
run-virtio-blk: $(IMAGE_NAME).iso $(TEST_DISK_IMG)
	qemu-system-x86_64 \
		-M q35 \
		-cdrom $(IMAGE_NAME).iso \
		-boot d \
		-drive file=$(TEST_DISK_IMG),if=none,id=vblkdrive,format=raw \
		-device virtio-blk-pci,drive=vblkdrive \
		$(QEMUFLAGS)

.PHONY: run-virtio-blk-uefi
run-virtio-blk-uefi: edk2-ovmf-bins $(IMAGE_NAME).iso $(TEST_DISK_IMG)
	qemu-system-x86_64 \
		-M q35 \
		-drive if=pflash,unit=0,format=raw,file=edk2-ovmf-bins/ovmf-code-x86_64.fd,readonly=on \
		-cdrom $(IMAGE_NAME).iso \
		-boot d \
		-drive file=$(TEST_DISK_IMG),if=none,id=vblkdrive,format=raw \
		-device virtio-blk-pci,drive=vblkdrive \
		$(QEMUFLAGS)

edk2-ovmf-bins:
	curl -L https://github.com/osdev0/edk2-ovmf-stable-bins/releases/latest/download/edk2-ovmf-bins.tar.gz | gunzip | tar -xf -

# ---------------------------------------------------------------------------
# Limine bootloader (host utility + prebuilt stage images)
# ---------------------------------------------------------------------------

limine-binary/limine:
	rm -rf limine-binary
	curl -L https://github.com/Limine-Bootloader/Limine/releases/download/$(LIMINE_VERSION)/limine-binary.tar.gz | gunzip | tar -xf -
	$(MAKE) -C limine-binary \
		CC="$(HOST_CC)" \
		CFLAGS="$(HOST_CFLAGS)" \
		CPPFLAGS="$(HOST_CPPFLAGS)" \
		LDFLAGS="$(HOST_LDFLAGS)" \
		LIBS="$(HOST_LIBS)"

# ---------------------------------------------------------------------------
# Kernel
# ---------------------------------------------------------------------------

kernel/.deps-obtained:
	./kernel/get-deps

.PHONY: kernel
kernel: kernel/.deps-obtained
	$(MAKE) -C kernel

# ---------------------------------------------------------------------------
# Userland demo programs + the initrd that ships them to the kernel
# ---------------------------------------------------------------------------

.PHONY: userland
userland:
	$(MAKE) -C userland

INITRD_PROGRAMS := hello sysinfo guess nsh mkfile splitdemo multisplit

# A plain ustar archive of userland/bin/* under /bin -- unpacked into
# tmpfs at boot by fs/initrd.c. `--format=ustar` is load-bearing: GNU
# tar's *default* format ("gnu") isn't what the kernel's parser expects.
initrd.tar: userland
	rm -rf initrd_root
	mkdir -p initrd_root/bin
	cp -v $(addprefix userland/bin/,$(INITRD_PROGRAMS)) initrd_root/bin/
	tar --format=ustar -C initrd_root -cf initrd.tar bin
	rm -rf initrd_root

# ---------------------------------------------------------------------------
# ISO image (bootable on both legacy BIOS and UEFI)
# ---------------------------------------------------------------------------

$(IMAGE_NAME).iso: limine-binary/limine kernel initrd.tar
	rm -rf iso_root
	mkdir -p iso_root/boot
	cp -v kernel/bin/kernel iso_root/boot/
	cp -v initrd.tar iso_root/boot/
	mkdir -p iso_root/boot/limine
	cp -v limine.conf limine-binary/limine-bios.sys limine-binary/limine-bios-cd.bin limine-binary/limine-uefi-cd.bin iso_root/boot/limine/
	mkdir -p iso_root/EFI/BOOT
	cp -v limine-binary/BOOTX64.EFI iso_root/EFI/BOOT/
	cp -v limine-binary/BOOTIA32.EFI iso_root/EFI/BOOT/
	xorriso -as mkisofs -R -r -J -b boot/limine/limine-bios-cd.bin \
		-no-emul-boot -boot-load-size 4 -boot-info-table -hfsplus \
		-apm-block-size 2048 --efi-boot boot/limine/limine-uefi-cd.bin \
		-efi-boot-part --efi-boot-image --protective-msdos-label \
		iso_root -o $(IMAGE_NAME).iso
	./limine-binary/limine bios-install $(IMAGE_NAME).iso
	rm -rf iso_root

# ---------------------------------------------------------------------------
# Raw GPT disk/USB image (bootable on both legacy BIOS and UEFI).
# This is the one you `dd` straight to a USB stick for real hardware.
# ---------------------------------------------------------------------------

$(IMAGE_NAME).hdd: limine-binary/limine kernel initrd.tar
	rm -f $(IMAGE_NAME).hdd
	dd if=/dev/zero bs=1M count=0 seek=64 of=$(IMAGE_NAME).hdd
	PATH=$$PATH:/usr/sbin:/sbin sgdisk $(IMAGE_NAME).hdd -n 1:2048 -t 1:ef00 -m 1
	./limine-binary/limine bios-install $(IMAGE_NAME).hdd
	mformat -i $(IMAGE_NAME).hdd@@1M
	mmd -i $(IMAGE_NAME).hdd@@1M ::/EFI ::/EFI/BOOT ::/boot ::/boot/limine
	mcopy -i $(IMAGE_NAME).hdd@@1M kernel/bin/kernel ::/boot
	mcopy -i $(IMAGE_NAME).hdd@@1M initrd.tar ::/boot
	mcopy -i $(IMAGE_NAME).hdd@@1M limine.conf limine-binary/limine-bios.sys ::/boot/limine
	mcopy -i $(IMAGE_NAME).hdd@@1M limine-binary/BOOTX64.EFI ::/EFI/BOOT
	mcopy -i $(IMAGE_NAME).hdd@@1M limine-binary/BOOTIA32.EFI ::/EFI/BOOT

.PHONY: clean
clean:
	$(MAKE) -C kernel clean
	$(MAKE) -C userland clean
	rm -rf iso_root initrd_root initrd.tar $(IMAGE_NAME).iso $(IMAGE_NAME).hdd

.PHONY: distclean
distclean: clean
	$(MAKE) -C kernel distclean
	rm -rf limine-binary edk2-ovmf-bins kernel/.deps-obtained
