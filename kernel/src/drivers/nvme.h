#ifndef NEXUS_NVME_H
#define NEXUS_NVME_H

#include "klib/klib.h"

/* Scans for the first NVMe controller, brings up its admin + one I/O
 * queue, and identifies namespace 1. Never panics on failure. */
bool nvme_init(void);

bool nvme_available(void);

uint64_t nvme_sector_count(void);
uint32_t nvme_sector_size(void);

/* buf must be page-aligned and physically contiguous (pmm_alloc_pages()/
 * kmalloc() -- not a stack buffer spanning a page boundary). */
bool nvme_read(uint64_t lba, uint32_t count, void *buf);
bool nvme_write(uint64_t lba, uint32_t count, const void *buf);

#endif /* NEXUS_NVME_H */
