#ifndef NEXUS_BOX8X8_H
#define NEXUS_BOX8X8_H

#include "klib/klib.h"

/*
 * A small, second 8x8 bitmap table alongside nx_font8x8.h -- just the
 * 22 box-drawing characters (light + double line) the shell's TUI
 * chrome (borders, dividers, box-drawn banners) wants: U+2500-U+253C
 * (light) and U+2550-U+256C (double).
 *
 * This is deliberately NOT a step toward general Unicode text
 * support. Nexus's whole text pipeline -- kprintf()/kvsnprintf()
 * (klib/printf.c), console_putc()/console_puts() (video/console.c),
 * even keyboard input (drivers/keyboard.c) -- is single-byte/ASCII
 * throughout, with no UTF-8 decoder anywhere. Building one just to
 * let a handful of box-drawing glyphs flow through kprintf("%s", ...)
 * would be a large, separate undertaking (multi-byte sequence
 * decoding, combining characters, width -- box-drawing is the easy
 * 1-column case, general Unicode is not) for very little payoff here.
 * Instead, these 22 glyphs are addressable directly, by name, via
 * console_putc_box()/console_putc_box_at() (video/console.h) --
 * exactly the primitive shell/shell.c's box-drawing helpers
 * (print_box_border(), print_divider(), ...) actually need, with no
 * byte-stream decoding involved at all.
 *
 * nx_box_glyph_from_codepoint() exists purely so that IF a real UTF-8
 * decoder ever gets bolted onto the input side of this pipeline, the
 * glyph data and the "which Unicode codepoint is this" mapping are
 * already sitting right here, ready to be wired up -- not because
 * anything in this kernel currently produces or consumes UTF-8.
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
