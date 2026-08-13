#ifndef NEXUS_NVME_H
#define NEXUS_NVME_H

#include "klib/klib.h"

/* Minimal NVMe driver: brings up the controller's admin queue, creates
 * one I/O submission/completion queue pair, and identifies namespace 1.
 * Polled throughout -- no MSI-X wiring yet, even though drivers/pci.h
 * already has the machinery for it. Deliberate sequencing: prove the
 * queue mechanics (PRP building, doorbells, completion parsing) are
 * correct against a synchronous poll loop first, where a bug just hangs
 * this one request instead of silently misrouting an interrupt. Once
 * that's proven, swapping the poll loop in submit_and_wait() for a
 * wait_queue_block()/wake() pair fed by a real MSI-X handler is a
 * contained, separate change.
 *
 * v1 also deliberately doesn't: support more than one namespace, chain
 * multiple PRP-list pages (caps a single request at ~2MiB), flush,
 * or do NVMe-specific error recovery beyond "log and fail the
 * request".
 */

/* Scans for the first NVMe controller (PCI class 0x01/0x08/0x02),
 * brings it up, and identifies namespace 1. Returns false (having
 * logged why) if no controller was found or bring-up failed at any
 * step -- never panics; a missing/misbehaving disk shouldn't take the
 * kernel down, same as a missing initrd module. */
bool nvme_init(void);

bool nvme_available(void);

uint64_t nvme_sector_count(void);
uint32_t nvme_sector_size(void);

/* Reads/writes `count` logical blocks starting at `lba` into/from
 * `buf`. `buf` must be page-aligned (v1 doesn't handle a PRP1 with a
 * sub-page offset) and physically contiguous per-page, which
 * pmm_alloc_pages()/kmalloc() both give you -- a stack buffer spanning
 * a page boundary is not guaranteed to. Returns false on a
 * controller-reported error, a timeout, or an out-of-range request. */
bool nvme_read(uint64_t lba, uint32_t count, void *buf);
bool nvme_write(uint64_t lba, uint32_t count, const void *buf);

#endif /* NEXUS_NVME_H */
