#include "klib/klib.h"

/*
 * A small, dependency-free printf core. Supports the subset that a kernel
 * actually needs:
 *
 *   %d %i   signed decimal
 *   %u      unsigned decimal
 *   %x %X   unsigned hex (lower/upper)
 *   %p      pointer (0x-prefixed hex)
 *   %s      string (NULL-safe: prints "(null)")
 *   %c      character
 *   %%      literal percent
 *
 * Width and zero-padding are supported for numeric conversions, e.g.
 * "%08x" or "%4d". No floating point -- the kernel has no business
 * formatting floats, and we build with -mno-sse anyway.
 *
 * Writes into a caller-supplied buffer like standard snprintf: always
 * NUL-terminates if size > 0, returns the number of characters that
 * *would* have been written (excluding the NUL) so truncation can be
 * detected the usual way.
 */

struct sink {
    char *buf;
    size_t size;   /* capacity of buf, including room for the NUL */
    size_t written; /* logical count, may exceed size (truncation) */
};

static void sink_putc(struct sink *s, char c) {
    if (s->written + 1 < s->size) {
        s->buf[s->written] = c;
    }
    s->written++;
}

static void sink_puts(struct sink *s, const char *str) {
    while (*str) {
        sink_putc(s, *str++);
    }
}

static void print_uint(struct sink *s, uint64_t val, unsigned base,
                        bool upper, int width, bool zero_pad) {
    static const char *digits_lower = "0123456789abcdef";
    static const char *digits_upper = "0123456789ABCDEF";
    const char *digits = upper ? digits_upper : digits_lower;

    char tmp[32];
    int n = 0;

    if (val == 0) {
        tmp[n++] = '0';
    } else {
        while (val > 0) {
            tmp[n++] = digits[val % base];
            val /= base;
        }
    }

    for (int pad = width - n; pad > 0; pad--) {
        sink_putc(s, zero_pad ? '0' : ' ');
    }

    while (n > 0) {
        sink_putc(s, tmp[--n]);
    }
}

static void print_int(struct sink *s, int64_t val, int width, bool zero_pad) {
    uint64_t uval;
    if (val < 0) {
        sink_putc(s, '-');
        uval = (uint64_t)(-(val + 1)) + 1; /* avoid UB on INT64_MIN */
        if (width > 0) {
            width--;
        }
    } else {
        uval = (uint64_t)val;
    }
    print_uint(s, uval, 10, false, width, zero_pad);
}

int kvsnprintf(char *buf, size_t size, const char *fmt, va_list ap) {
    struct sink s = { .buf = buf, .size = size, .written = 0 };

    for (const char *p = fmt; *p; p++) {
        if (*p != '%') {
            sink_putc(&s, *p);
            continue;
        }

        p++;
        if (*p == '\0') {
            break;
        }

        bool zero_pad = false;
        if (*p == '0') {
            zero_pad = true;
            p++;
        }

        int width = 0;
        while (*p >= '0' && *p <= '9') {
            width = width * 10 + (*p - '0');
            p++;
        }

        /* Length modifiers: we only care about distinguishing 64-bit from
         * the default int-sized argument, so 'l'/'ll'/'z' are all treated
         * the same (promoted varargs are at least int-sized anyway). */
        bool longarg = false;
        while (*p == 'l' || *p == 'z') {
            longarg = true;
            p++;
        }

        switch (*p) {
        case 'd':
        case 'i': {
            int64_t v = longarg ? va_arg(ap, int64_t) : va_arg(ap, int32_t);
            print_int(&s, v, width, zero_pad);
            break;
        }
        case 'u': {
            uint64_t v = longarg ? va_arg(ap, uint64_t) : va_arg(ap, uint32_t);
            print_uint(&s, v, 10, false, width, zero_pad);
            break;
        }
        case 'x': {
            uint64_t v = longarg ? va_arg(ap, uint64_t) : va_arg(ap, uint32_t);
            print_uint(&s, v, 16, false, width, zero_pad);
            break;
        }
        case 'X': {
            uint64_t v = longarg ? va_arg(ap, uint64_t) : va_arg(ap, uint32_t);
            print_uint(&s, v, 16, true, width, zero_pad);
            break;
        }
        case 'p': {
            uintptr_t v = (uintptr_t)va_arg(ap, void *);
            sink_puts(&s, "0x");
            print_uint(&s, v, 16, false, 16, true);
            break;
        }
        case 's': {
            const char *str = va_arg(ap, const char *);
            sink_puts(&s, str ? str : "(null)");
            break;
        }
        case 'c': {
            char c = (char)va_arg(ap, int);
            sink_putc(&s, c);
            break;
        }
        case '%':
            sink_putc(&s, '%');
            break;
        default:
            sink_putc(&s, '%');
            sink_putc(&s, *p);
            break;
        }
    }

    if (size > 0) {
        size_t end = s.written < size - 1 ? s.written : size - 1;
        buf[end] = '\0';
    }

    return (int)s.written;
}

int ksnprintf(char *buf, size_t size, const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    int ret = kvsnprintf(buf, size, fmt, ap);
    va_end(ap);
    return ret;
}
