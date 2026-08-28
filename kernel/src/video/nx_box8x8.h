#ifndef NEXUS_BOX8X8_H
#define NEXUS_BOX8X8_H

#include "klib/klib.h"

/*
 * 8x8 bitmap glyphs for the supported Unicode box-drawing characters.
 *
 * One byte represents each row; bit x corresponds to column x.
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

/* Maps a supported Unicode codepoint to a box glyph. */
bool nx_box_glyph_from_codepoint(uint32_t codepoint, enum nx_box_glyph *out);

#endif /* NEXUS_BOX8X8_H */
