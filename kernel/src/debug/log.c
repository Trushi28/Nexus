#include "debug/log.h"
#include "debug/serial.h"
#include "sync/spinlock.h"
#include "video/console.h"

#define KPRINTF_BUF_SIZE 768

static spinlock_t log_lock = SPINLOCK_INIT;

int kprintf(const char *fmt, ...) {
  char buf[KPRINTF_BUF_SIZE];

  va_list ap;
  va_start(ap, fmt);
  int len = kvsnprintf(buf, sizeof(buf), fmt, ap);
  va_end(ap);

  uint64_t flags = spinlock_acquire_irqsave(&log_lock);
  serial_puts(buf);
  console_puts(buf);
  spinlock_release_irqrestore(&log_lock, flags);

  if (len < 0 || (size_t)len >= sizeof(buf)) {
    char warn[96];
    ksnprintf(warn, sizeof(warn),
              "[log] WARNING: previous kprintf() truncated (wanted %d bytes, "
              "buffer is %u) -- split it into multiple calls\n",
              len, (unsigned)sizeof(buf));
    uint64_t f2 = spinlock_acquire_irqsave(&log_lock);
    serial_puts(warn);
    console_puts(warn);
    spinlock_release_irqrestore(&log_lock, f2);
  }

  return len;
}
