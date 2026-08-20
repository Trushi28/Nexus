#ifndef NEXUS_VIRTIO_BLK_H
#define NEXUS_VIRTIO_BLK_H

#include "klib/klib.h"

/*
 * A minimal virtio-blk driver over the MODERN virtio-pci transport
 * (spec 1.0+; no legacy port-I/O fallback). Second implementation
 * behind drivers/blockdev.h's HAL, alongside drivers/nvme.c -- fs/graph.c
 * doesn't know or care which one actually ends up registered.
 *
 * *** UNVERIFIED AGAINST REAL HARDWARE/QEMU ***
 * This was written and compile-tested against the virtio spec, but
 * this build environment has no way to actually boot it against
 * emulated (or real) virtio-blk hardware. Test with the top-level
 * GNUmakefile's `run-virtio-blk` target before trusting it with real
 * data -- treat every design note below tagged "VERIFY ON REAL QEMU"
 * as exactly that, not a settled fact.
 *
 * Deliberately conservative, mirroring drivers/nvme.c's own original
 * pre-MSI-X phase (prove the queue mechanics right against something
 * simple first): purely polled, no MSI-X, no interrupt registration
 * at all. If the device falls back to legacy INTx because MSI-X was
 * never configured, that IRQ line simply stays masked at the I/O
 * APIC (nothing here ever calls ioapic_set_irq() for it) -- the CPU
 * never sees it, so there's nothing to acknowledge and nothing to go
 * wrong. Also single-request-at-a-time: submit, poll the used ring
 * until it advances (bounded by a timeout), read the status byte,
 * return -- exactly matching how fs/graph.c's disk_op_lock() already
 * serializes every caller anyway, so there was never a reason to
 * support more than one in-flight request.
 *
 * v1 also deliberately doesn't: negotiate any optional feature bit
 * (RO, BLK_SIZE, FLUSH, TOPOLOGY, MQ, ...) -- accepts nothing beyond
 * VIRTIO_F_VERSION_1, which a modern-capable device is required to
 * offer for this driver to work with it at all; enforce size_max/
 * seg_max (fs/graph.c's largest transfer is 1MiB, comfortably under
 * any real device's defaults); or chain multiple data descriptors --
 * one descriptor covers the whole transfer, since (unlike NVMe's PRP
 * scheme) a virtqueue descriptor has no page-boundary restriction and
 * every buffer graph.c hands this driver is physically contiguous
 * (pmm_alloc_pages()) already.
 */

bool virtio_blk_init(void);

bool virtio_blk_available(void);
uint64_t virtio_blk_sector_count(void);
uint32_t virtio_blk_sector_size(void); /* always 512 -- the spec-fixed
                                           request unit, independent of
                                           any negotiated blk_size (not
                                           negotiated here) */
bool virtio_blk_read(uint64_t lba, uint32_t count, void *buf);
bool virtio_blk_write(uint64_t lba, uint32_t count, const void *buf);

#endif /* NEXUS_VIRTIO_BLK_H */
