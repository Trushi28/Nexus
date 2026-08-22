#ifndef NEXUS_FONT8X8_H
#define NEXUS_FONT8X8_H

#include "klib/klib.h"

/*
 * Nexus's own 8x8 bitmap font -- ASCII 0x20 ('space') through 0x7E
 * ('~'), hand-designed for this project rather than vendored from an
 * external source. There used to be a third-party header here
 * (dhepper/font8x8, fetched at build time by kernel/get-deps); it's
 * gone now, for two reasons: kernel/get-deps had no control over
 * whether that header used internal (`static`) or external linkage
 * for its array, and it turned out to be the latter -- fine as long
 * as exactly one .c file ever included it, but the moment a second
 * one did (video/splash.c, alongside video/console.c), the linker
 * correctly refused with "multiple definition of `font8x8_basic`".
 * Vendoring our own, single-definition font sidesteps that whole
 * class of problem AND means this asset is fully owned by the repo,
 * not re-fetched from a URL on every clean build.
 *
 * Storage: one byte per row, 8 rows per glyph, top row first. Bit x
 * (1 << x) set means "pixel on" at column x, x=0 being the LEFTMOST
 * column -- i.e. bit 0 is NOT the traditional MSB-on-the-left order
 * you'd get from just reading the byte's bits left-to-right. This
 * matches video/console.c's existing draw_glyph() (`bits & (1 << x)`
 * for x = 0..7), which predates this file and was never changed --
 * only where the bytes themselves come from changed.
 *
 * Anything outside 0x20-0x7E (every C0 control code, and 0x7F DEL)
 * is an all-zero/blank glyph -- safe, since nothing in this kernel
 * ever asks to actually draw one: console_putc() intercepts '\n',
 * '\r', '\t', '\b' before a glyph lookup happens at all (see
 * video/console.c), and every other caller only ever feeds this
 * printable text.
 */
extern const uint8_t nx_font8x8[128][8];

#endif /* NEXUS_FONT8X8_H */
