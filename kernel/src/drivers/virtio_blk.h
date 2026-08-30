#ifndef NEXUS_VIRTIO_BLK_H
#define NEXUS_VIRTIO_BLK_H

#include "klib/klib.h"

/* Minimal modern-transport (spec 1.0+) virtio-blk driver, polled/no
 * MSI-X. Compile-tested only -- not yet verified against real QEMU. */

bool virtio_blk_init(void);

bool virtio_blk_available(void);
uint64_t virtio_blk_sector_count(void);
uint32_t virtio_blk_sector_size(void); // always 512, spec-fixed

bool virtio_blk_read(uint64_t lba, uint32_t count, void *buf);
bool virtio_blk_write(uint64_t lba, uint32_t count, const void *buf);

#endif /* NEXUS_VIRTIO_BLK_H */
