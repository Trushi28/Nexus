#include "drivers/blockdev.h"
#include "debug/log.h"
#include "sched/sched.h"

static const struct blockdev_ops *active_ops;
static const char *active_name;

bool blockdev_register(const char *name, const struct blockdev_ops *ops) {
  if (active_ops != NULL) {
    kprintf("[blockdev] refusing to register '%s' -- '%s' is already the "
            "active block device (v1 supports exactly one at a time)\n",
            name, active_name);
    return false;
  }
  active_ops = ops;
  active_name = name;
  kprintf("[blockdev] '%s' is now the active block device\n", name);
  return true;
}

bool blockdev_available(void) { return active_ops != NULL; }

uint64_t blockdev_sector_count(void) {
  return (active_ops != NULL) ? active_ops->sector_count() : 0;
}

uint32_t blockdev_sector_size(void) {
  return (active_ops != NULL) ? active_ops->sector_size() : 0;
}

/* CAS-spin-and-yield, not a spinlock -- this can block on a completion
 * interrupt, and holding a spinlock across that would disable IRQs
 * on this cpu for as long as the disk takes to answer. */
static volatile bool io_busy = false;

static void io_lock(void) {
  for (;;) {
    bool expected = false;
    if (__atomic_compare_exchange_n(&io_busy, &expected, true, false,
                                    __ATOMIC_ACQUIRE, __ATOMIC_RELAXED)) {
      return;
    }
    sched_sleep_ms(5);
  }
}

static void io_unlock(void) {
  __atomic_store_n(&io_busy, false, __ATOMIC_RELEASE);
}

bool blockdev_read(uint64_t lba, uint32_t count, void *buf) {
  if (active_ops == NULL) {
    return false;
  }
  io_lock();
  bool ok = active_ops->read(lba, count, buf);
  io_unlock();
  return ok;
}

bool blockdev_write(uint64_t lba, uint32_t count, const void *buf) {
  if (active_ops == NULL) {
    return false;
  }
  io_lock();
  bool ok = active_ops->write(lba, count, buf);
  io_unlock();
  return ok;
}
