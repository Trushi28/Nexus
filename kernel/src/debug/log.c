#include "debug/log.h"
#include "debug/serial.h"
#include "video/console.h"
#include "sync/spinlock.h"

static spinlock_t log_lock = SPINLOCK_INIT;

int kprintf(const char *fmt, ...) {
    char buf[512];

    va_list ap;
    va_start(ap, fmt);
    int len = kvsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);

    uint64_t flags = spinlock_acquire_irqsave(&log_lock);
    serial_puts(buf);
    console_puts(buf);
    spinlock_release_irqrestore(&log_lock, flags);

    return len;
}
