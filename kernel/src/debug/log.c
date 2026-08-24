#include "debug/log.h"
#include "debug/serial.h"
#include "sync/spinlock.h"
#include "video/console.h"

#define KPRINTF_BUF_SIZE 768

static spinlock_t log_lock = SPINLOCK_INIT;

static int kvprintf_raw(const char *fmt, va_list ap) {
  char buf[KPRINTF_BUF_SIZE];

  int len = kvsnprintf(buf, sizeof(buf), fmt, ap);

  serial_puts(buf);
  console_puts(buf);

  if (len < 0 || (size_t)len >= sizeof(buf)) {
    char warn[96];
    ksnprintf(warn, sizeof(warn),
              "[log] WARNING: previous kprintf() truncated (wanted %d bytes, "
              "buffer is %u) -- split it into multiple calls\n",
              len, (unsigned)sizeof(buf));
    serial_puts(warn);
    console_puts(warn);
  }

  return len;
}

int kprintf(const char *fmt, ...) {
  va_list ap;
  va_start(ap, fmt);

  uint64_t flags = spinlock_acquire_irqsave(&log_lock);
  int len = kvprintf_raw(fmt, ap);
  spinlock_release_irqrestore(&log_lock, flags);

  va_end(ap);
  return len;
}

int kprintf_locked(const char *fmt, ...) {
  va_list ap;
  va_start(ap, fmt);
  int len = kvprintf_raw(fmt, ap);
  va_end(ap);
  return len;
}

uint64_t kprintf_lock_acquire(void) {
  return spinlock_acquire_irqsave(&log_lock);
}

void kprintf_lock_release(uint64_t flags) {
  spinlock_release_irqrestore(&log_lock, flags);
}
