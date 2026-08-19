#include "drivers/blockdev.h"
#include "debug/log.h"

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

bool blockdev_read(uint64_t lba, uint32_t count, void *buf) {
  return active_ops != NULL && active_ops->read(lba, count, buf);
}

bool blockdev_write(uint64_t lba, uint32_t count, const void *buf) {
  return active_ops != NULL && active_ops->write(lba, count, buf);
}
