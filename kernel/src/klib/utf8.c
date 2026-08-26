#include "klib/utf8.h"

/* RFC 3629 lead-byte shapes -- exactly four of them, so this is
 * expressed as a handful of mask checks rather than a lookup table:
 *   0xxxxxxx                            1 byte,  codepoint 0-0x7F
 *   110xxxxx 10xxxxxx                   2 bytes, codepoint 0x80-0x7FF
 *   1110xxxx 10xxxxxx 10xxxxxx          3 bytes, codepoint 0x800-0xFFFF
 *   11110xxx 10xxxxxx 10xxxxxx 10xxxxxx 4 bytes, codepoint 0x10000-0x10FFFF
 * `min_cp` per length is what rejects an overlong encoding -- e.g. a
 * 2-byte sequence spelling out a codepoint that fits in 1 byte, which
 * is valid-looking bit pattern but not valid UTF-8 (RFC 3629 section
 * 3 forbids it -- a security-relevant rule in general, since overlong
 * encodings have historically been used to smuggle bytes past naive
 * ASCII-based filters; not a real concern for this kernel's use case,
 * but rejecting them is also just what "valid UTF-8" means). */
uint32_t utf8_decode(const char *s, size_t len, size_t *consumed) {
  if (len == 0) {
    *consumed = 0;
    return UTF8_INVALID;
  }

  uint8_t b0 = (uint8_t)s[0];

  if (b0 < 0x80) {
    *consumed = 1;
    return b0;
  }

  size_t extra;
  uint32_t cp;
  uint32_t min_cp;

  if ((b0 & 0xE0) == 0xC0) {
    extra = 1;
    cp = b0 & 0x1F;
    min_cp = 0x80;
  } else if ((b0 & 0xF0) == 0xE0) {
    extra = 2;
    cp = b0 & 0x0F;
    min_cp = 0x800;
  } else if ((b0 & 0xF8) == 0xF0) {
    extra = 3;
    cp = b0 & 0x07;
    min_cp = 0x10000;
  } else {
    *consumed = 1; /* a stray continuation byte (10xxxxxx), or a
                       pattern UTF-8 never uses at all (0xF8-0xFF) */
    return UTF8_INVALID;
  }

  if (extra >= len) {
    *consumed = 1; /* truncated -- fewer bytes available than the
                       lead byte promised */
    return UTF8_INVALID;
  }

  for (size_t i = 1; i <= extra; i++) {
    uint8_t b = (uint8_t)s[i];
    if ((b & 0xC0) != 0x80) {
      *consumed = 1; /* not a continuation byte where one was required */
      return UTF8_INVALID;
    }
    cp = (cp << 6) | (b & 0x3F);
  }

  if (cp < min_cp || cp > 0x10FFFF || (cp >= 0xD800 && cp <= 0xDFFF)) {
    *consumed = 1; /* overlong encoding, out of Unicode's range, or an
                       encoded UTF-16 surrogate half -- none valid */
    return UTF8_INVALID;
  }

  *consumed = extra + 1;
  return cp;
}
