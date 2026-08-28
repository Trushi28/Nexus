#include "cpu/usercopy.h"

extern size_t __copy_from_user_asm(void *dst, const void *src, size_t len);
extern size_t __copy_to_user_asm(void *dst, const void *src, size_t len);

size_t copy_from_user(void *kdst, const void *usrc, size_t len) {
  return __copy_from_user_asm(kdst, usrc, len);
}

size_t copy_to_user(void *udst, const void *ksrc, size_t len) {
  return __copy_to_user_asm(udst, ksrc, len);
}

bool copy_string_from_user(char *kdst, const void *usrc, size_t max) {
  if (max == 0) {
    return false;
  }
  const uint8_t *up = (const uint8_t *)usrc;
  for (size_t i = 0; i < max - 1; i++) {
    uint8_t c;
    if (copy_from_user(&c, up + i, 1) != 0) {
      return false; /* faulted before we found a terminator */
    }
    kdst[i] = (char)c;
    if (c == '\0') {
      return true;
    }
  }
  kdst[max - 1] = '\0'; /* silently truncated, same as the strncpy() call sites this replaces */
  return true;
}
