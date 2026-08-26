#ifndef NEXUS_BOX8X8_H
#define NEXUS_BOX8X8_H

#include "klib/klib.h"

/*
 * A small, second 8x8 bitmap table alongside nx_font8x8.h -- just the
 * 22 box-drawing characters (light + double line) the shell's TUI
 * chrome (borders, dividers, box-drawn banners) wants: U+2500-U+253C
 * (light) and U+2550-U+256C (double).
 *
 * This is NOT a step toward general Unicode text SHAPING support --
 * still no combining characters, no bidi, no wide/fullwidth glyph
 * width accounting. It IS, as of klib/utf8.c, wired up to a real
 * (if deliberately minimal) UTF-8 decoder: console_puts()
 * (video/console.c) -- what every kprintf("%s", ...) funnels a
 * formatted string through in one shot -- decodes multi-byte
 * sequences and renders anything matching one of these 22 codepoints
 * through this table, falling back to a plain '?' for anything else.
 * console_putc() itself stays a raw single-byte pass-through, since
 * every direct caller of it only ever hands it one already-known
 * ASCII byte (a keystroke off the PS/2 keyboard, which can't produce
 * anything >= 0x80 at all -- see drivers/keyboard.c). These 22 glyphs
 * are ALSO still addressable directly, by name, via
 * console_putc_box()/console_putc_box_at() (video/console.h) -- the
 * primitive shell/shell.c's box-drawing helpers (print_box_border(),
 * print_divider(), ...) use, with no byte-stream decoding involved at
 * all, since they're building UI chrome from a fixed enum, not
 * printing arbitrary text that might happen to contain one of these.
 *
 * nx_box_glyph_from_codepoint() is what console_puts() (via
 * klib/utf8.h's utf8_decode()) actually calls now -- see that
 * function's own comment for the fuller story of why this mattered
 * (GraphFS content is arbitrary bytes; catting a file with real
 * Unicode in it used to render every byte of a multi-byte sequence as
 * its own garbled placeholder instead of one correct glyph).
 *
 * Storage/bit convention: identical to nx_font8x8.h -- one byte per
 * row (top to bottom), bit x (1 << x) set means "pixel on" at column
 * x (0 = leftmost).
 */

enum nx_box_glyph {
  /* light (single-line) -- U+2500 block */
  NX_BOX_V,  /* U+2502 │ vertical                */
  NX_BOX_H,  /* U+2500 ─ horizontal              */
  NX_BOX_DR, /* U+250C ┌ down + right (top-left) */
  NX_BOX_DL, /* U+2510 ┐ down + left (top-right) */
  NX_BOX_UR, /* U+2514 └ up + right (bot-left)   */
  NX_BOX_UL, /* U+2518 ┘ up + left (bot-right)   */
  NX_BOX_VR, /* U+251C ├ vertical + right        */
  NX_BOX_VL, /* U+2524 ┤ vertical + left         */
  NX_BOX_HD, /* U+252C ┬ horizontal + down       */
  NX_BOX_HU, /* U+2534 ┴ horizontal + up         */
  NX_BOX_VH, /* U+253C ┼ cross                   */

  /* double-line -- U+2550 block */
  NX_BOX_DBL_V,  /* U+2551 ║ */
  NX_BOX_DBL_H,  /* U+2550 ═ */
  NX_BOX_DBL_DR, /* U+2554 ╔ */
  NX_BOX_DBL_DL, /* U+2557 ╗ */
  NX_BOX_DBL_UR, /* U+255A ╚ */
  NX_BOX_DBL_UL, /* U+255D ╝ */
  NX_BOX_DBL_VR, /* U+2560 ╠ */
  NX_BOX_DBL_VL, /* U+2563 ╣ */
  NX_BOX_DBL_HD, /* U+2566 ╦ */
  NX_BOX_DBL_HU, /* U+2569 ╩ */
  NX_BOX_DBL_VH, /* U+256C ╬ */

  NX_BOX_GLYPH_COUNT
};

extern const uint8_t nx_box8x8[NX_BOX_GLYPH_COUNT][8];

/* Maps a real Unicode codepoint (e.g. 0x2502) to its enum value.
 * False if `codepoint` isn't one of the 22 glyphs this table covers.
 * See the file header comment -- nothing upstream of this function
 * currently produces a codepoint to feed it. */
bool nx_box_glyph_from_codepoint(uint32_t codepoint, enum nx_box_glyph *out);

#endif /* NEXUS_BOX8X8_H */
