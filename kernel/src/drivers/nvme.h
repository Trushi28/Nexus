#ifndef NEXUS_NVME_H
#define NEXUS_NVME_H

#include "klib/klib.h"

/* Minimal NVMe driver: brings up the controller's admin queue, creates
 * one I/O submission/completion queue pair, and identifies namespace 1.
 * Command completion is MSI-X-driven (admin queue on vector 0, I/O
 * queue on vector 1): submit_and_wait() registers on the queue's
 * wait_queue and task_block()s, and the ISR (nvme_admin_irq_handler()/
 * nvme_io_irq_handler() in nvme.c) wakes it -- the request-blocks-a-
 * task model every other blocking primitive in this kernel uses, not
 * a poll loop. There's no timeout on that wait, consistent with the
 * rest of the kernel's blocking waits.
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
