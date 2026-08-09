#ifndef NEXUS_KLIB_H
#define NEXUS_KLIB_H

/*
 * Common kernel-wide includes and helper macros. <stdint.h>, <stddef.h>,
 * <stdbool.h> and <stdarg.h> are all part of the "freestanding" subset of
 * the C standard library that the compiler itself provides (no libc
 * needed) -- see C11 4/6.
 */

#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>
#include <stdarg.h>

#define PACKED       __attribute__((packed))
#define ALIGNED(x)   __attribute__((aligned(x)))
#define NORETURN     __attribute__((noreturn))
#define UNUSED       __attribute__((unused))
#define MAYBE_UNUSED __attribute__((unused))

#define PAGE_SIZE     0x1000UL
#define PAGE_MASK     (PAGE_SIZE - 1)

#define ALIGN_UP(x, a)   (((x) + ((a) - 1)) & ~((a) - 1))
#define ALIGN_DOWN(x, a) ((x) & ~((a) - 1))
#define DIV_ROUND_UP(x, a) (((x) + ((a) - 1)) / (a))

#define MIN(a, b) ((a) < (b) ? (a) : (b))
#define MAX(a, b) ((a) > (b) ? (a) : (b))

#define ARRAY_LEN(a) (sizeof(a) / sizeof((a)[0]))

#define container_of(ptr, type, member) \
    ((type *)((uint8_t *)(ptr) - offsetof(type, member)))

/* klib/string.c -- freestanding mem-family / str-family helpers. The
 * compiler is legally allowed to emit calls to memcpy/memset/memmove/
 * memcmp on its own even in -ffreestanding mode (e.g. for struct
 * assignment or array init), so these must exist with exactly these
 * names no matter what. */
void *memcpy(void *dest, const void *src, size_t n);
void *memset(void *s, int c, size_t n);
void *memmove(void *dest, const void *src, size_t n);
int   memcmp(const void *s1, const void *s2, size_t n);

size_t strlen(const char *s);
int    strcmp(const char *a, const char *b);
int    strncmp(const char *a, const char *b, size_t n);
char  *strcpy(char *dst, const char *src);
char  *strncpy(char *dst, const char *src, size_t n);
const char *strchr(const char *s, int c);
const char *strstr(const char *haystack, const char *needle);
bool   str_has_prefix(const char *s, const char *prefix);
bool   str_has_suffix(const char *s, const char *suffix);

/* klib/printf.c */
int kvsnprintf(char *buf, size_t size, const char *fmt, va_list ap);
int ksnprintf(char *buf, size_t size, const char *fmt, ...);

#endif /* NEXUS_KLIB_H */
