#ifndef NEXUS_UTF8_H
#define NEXUS_UTF8_H

#include "klib/klib.h"

/*
 * Allocation-free UTF-8 decoder.
 * Decodes one codepoint at a time without performing Unicode shaping.
 */

#define UTF8_INVALID 0xFFFFFFFFu

/*
 * Decodes one UTF-8 codepoint from `s`, with `len` bytes available.
 * Returns UTF8_INVALID for an invalid sequence. `*consumed` is always
 * set to at least 1 so callers can continue past invalid input.
 */
uint32_t utf8_decode(const char *s, size_t len, size_t *consumed);

#endif /* NEXUS_UTF8_H */
