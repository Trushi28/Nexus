#ifndef NEXUS_BLOCKDEV_H
#define NEXUS_BLOCKDEV_H

#include "klib/klib.h"

/*
 * A tiny hardware-abstraction layer over "a thing you can read/write
 * fixed-size sectors from/to by LBA" -- NVMe today, virtio-blk as a
 * fallback, anything else tomorrow, all behind the exact same four
 * calls. fs/graph.c's persistence layer (graph_save_to_disk()/
 * graph_load_from_disk()) goes through this instead of calling
 * drivers/nvme.h or drivers/virtio_blk.h directly, so it never needs
 * to change when a second (or third) driver shows up -- that driver
 * just registers itself here.
 *
 * Exactly one block device is registered at a time in v1: there's no
 * multi-disk support anywhere above this layer (no device
 * enumeration, no naming beyond a single active device), so a second
 * registration attempt is refused rather than silently replacing the
 * first. Whoever calls *_init() first and succeeds wins -- see
 * main.c's boot sequence.
 *
 * blockdev_read()/blockdev_write() serialize themselves against
 * concurrent callers -- see their own comment below for why that has
 * to live at THIS layer, not be left to whoever happens to call them.
 */

struct blockdev_ops {
  uint64_t (*sector_count)(void);
  uint32_t (*sector_size)(void);

  /* Reads/writes `count` logical blocks starting at `lba` into/from
   * `buf`. Same buffer-alignment contract as whatever the underlying
   * driver requires (e.g. drivers/nvme.h's page-alignment rule) --
   * this layer doesn't add or relax any constraint of its own. */
  bool (*read)(uint64_t lba, uint32_t count, void *buf);
  bool (*write)(uint64_t lba, uint32_t count, const void *buf);
};

/* Called once by a driver's own *_init() after it's confirmed a
 * working controller -- this is registration, not driver detection.
 * Returns false (having logged why) if a block device is already
 * registered. `ops` must outlive the registration (a static const
 * table in the driver's own .c file, same pattern as vnode_ops). */
bool blockdev_register(const char *name, const struct blockdev_ops *ops);

bool blockdev_available(void);
uint64_t blockdev_sector_count(void);
uint32_t blockdev_sector_size(void);

/* Reads/writes `count` logical blocks starting at `lba`, exactly like
 * the registered driver's own read()/write() (struct blockdev_ops
 * above) -- but with one guarantee neither NVMe nor virtio-blk's own
 * driver provides on its own: mutual exclusion against any other
 * concurrent caller of EITHER function, kernel-wide.
 *
 * Both drivers behind this HAL keep a single, reused, non-reentrant
 * piece of state for an in-flight request -- NVMe's per-queue
 * next_cid/sq_tail/cq_head (drivers/nvme.c), virtio-blk's one fixed
 * 3-descriptor chain and request/status buffers
 * (drivers/virtio_blk.c) -- because neither was ever built to have
 * two requests in flight at once. A second caller racing the first
 * doesn't queue politely behind it; it corrupts whichever of those
 * fields it touches concurrently. That risk exists the moment there's
 * more than one task that might ever call into this HAL --
 * fs/graph.c's autosave task racing a shell-driven gsync/gload is the
 * first such case, but the guarantee lives HERE, not in fs/graph.c,
 * so every future caller inherits it automatically instead of needing
 * to know a lock exists at all.
 *
 * A CAS-spin-and-yield gate, not a spinlock: this can (and for NVMe,
 * always does) block the calling task waiting on a completion
 * interrupt, and holding a spinlock across a blocking call would
 * leave interrupts disabled on this cpu for as long as the disk takes
 * to answer. Not a real sleeping mutex either -- this kernel doesn't
 * have one yet, and the contention here is still narrow enough (the
 * autosave task and whichever single shell task issues a gsync/gload)
 * that building one just for this isn't justified yet. */
bool blockdev_read(uint64_t lba, uint32_t count, void *buf);
bool blockdev_write(uint64_t lba, uint32_t count, const void *buf);

#endif /* NEXUS_BLOCKDEV_H */
