#ifndef NEXUS_UTF8_H
#define NEXUS_UTF8_H

#include "klib/klib.h"

/* A small, allocation-free UTF-8 decoder -- turns a byte stream into
 * Unicode codepoints, one at a time. Exists because this kernel's
 * whole text pipeline (console_putc()/console_puts(), kprintf(),
 * keyboard input) is single-byte/ASCII throughout with no multi-byte
 * awareness at all -- see video/nx_font8x8.h's and video/nx_box8x8.h's
 * own header comments, the latter of which explicitly anticipated
 * this module ("nx_box_glyph_from_codepoint() exists purely so that
 * IF a real UTF-8 decoder ever gets bolted onto the input side of
 * this pipeline..."). That mattered as soon as anything real could
 * contain multi-byte UTF-8: GraphFS content is arbitrary bytes (see
 * fs/graph.c), so `cat`/`gcat`-ing a file someone wrote with real
 * Unicode in it -- box-drawing characters pasted in from elsewhere,
 * say -- used to print each individual byte of a multi-byte sequence
 * as its own garbled glyph (every byte >= 0x80 falls outside
 * nx_font8x8's 0x20-0x7E coverage and rendered as a lone '?' each,
 * per draw_glyph()'s existing out-of-range fallback -- so one real
 * character came out as two-to-four '?'s). See video/console.c's
 * console_puts() for where this actually gets used.
 *
 * Deliberately NOT a general-purpose Unicode text-shaping engine --
 * no combining characters, no bidi, no wide/fullwidth glyph-width
 * accounting. It decodes exactly one thing: "how many bytes does
 * this codepoint take, and what is it" -- valid UTF-8 (RFC 3629: 1-4
 * byte sequences, no lone surrogates, no overlong encodings) or a
 * single-byte "invalid" marker for anything else. Callers render
 * whatever placeholder they think is appropriate for an
 * unrecognized/invalid codepoint -- this module doesn't pick one.
 */

#define UTF8_INVALID 0xFFFFFFFFu

/* Decodes ONE codepoint starting at `s`, with `len` bytes available
 * (NOT necessarily NUL-terminated -- callers pass their own known
 * length, e.g. via strlen()). Returns the decoded codepoint, or
 * UTF8_INVALID if `s[0]` doesn't start a valid UTF-8 sequence (a
 * stray continuation byte, a byte pattern UTF-8 never uses, a
 * sequence that would run past `len`, a continuation byte missing
 * where one was required, an overlong encoding, a codepoint outside
 * Unicode's valid range, or an encoded UTF-16 surrogate half -- none
 * of which are legal UTF-8). Always writes at least 1 to `*consumed`,
 * even on failure, so a caller looping over a byte stream always
 * makes forward progress and can't spin forever on one bad byte. */
uint32_t utf8_decode(const char *s, size_t len, size_t *consumed);

#endif /* NEXUS_UTF8_H */
