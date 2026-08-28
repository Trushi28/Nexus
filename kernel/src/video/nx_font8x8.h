#ifndef NEXUS_FONT8X8_H
#define NEXUS_FONT8X8_H

#include "klib/klib.h"

/*
 * 8x8 bitmap font for ASCII characters.
 *
 * Each glyph has 8 rows. Bit x set means the pixel at column x is on.
 * Non-printable characters are blank.
 */

extern const uint8_t nx_font8x8[128][8];

#endif /* NEXUS_FONT8X8_H */
