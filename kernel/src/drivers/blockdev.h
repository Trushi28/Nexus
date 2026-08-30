#ifndef NEXUS_BLOCKDEV_H
#define NEXUS_BLOCKDEV_H

#include "klib/klib.h"

/* Tiny HAL over "read/write fixed-size sectors by LBA" -- NVMe or
 * virtio-blk register here; fs/graph.c never calls either directly. */

struct blockdev_ops {
  uint64_t (*sector_count)(void);
  uint32_t (*sector_size)(void);
  bool (*read)(uint64_t lba, uint32_t count, void *buf);
  bool (*write)(uint64_t lba, uint32_t count, const void *buf);
};

/* Registers a driver's ops table after it's confirmed a working
 * controller. Only one device at a time -- a second call is refused. */
bool blockdev_register(const char *name, const struct blockdev_ops *ops);

bool blockdev_available(void);
uint64_t blockdev_sector_count(void);
uint32_t blockdev_sector_size(void);

/* Same contract as the registered driver's own read()/write(), plus
 * mutual exclusion against any other concurrent caller -- both drivers
 * behind this HAL keep single, non-reentrant in-flight request state. */
bool blockdev_read(uint64_t lba, uint32_t count, void *buf);
bool blockdev_write(uint64_t lba, uint32_t count, const void *buf);

#endif /* NEXUS_BLOCKDEV_H */
