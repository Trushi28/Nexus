#ifndef NEXUS_CONSOLE_H
#define NEXUS_CONSOLE_H

#include "klib/klib.h"
#include "video/nx_box8x8.h"

// Shared console/UI colors.
#define NX_COLOR_BG 0x000B0E14
#define NX_COLOR_FG 0x00E0E0E0
#define NX_COLOR_ACCENT 0x0057C7FF
#define NX_COLOR_DIM 0x00445566
#define NX_COLOR_ERROR 0x00FF6B6B

void console_init(void);
void console_clear(void);

// Writes one raw byte at the cursor.
void console_putc(char c);

// Writes a UTF-8 string. Supported box-drawing characters are rendered as glyphs; unsupported sequences render as '?'.
void console_puts(const char *s);
void console_set_colors(uint32_t fg, uint32_t bg);

// Handles backspace by erasing the previous glyph cell, used by the shell's line editor. No-op at column 0 of a fresh line.
void console_backspace(void);

// The console's size in character cells (not pixels). Both read 0 if there's no framebuffer.
uint32_t console_cols(void);
uint32_t console_rows(void);

/* Draws a glyph at (col, row) without moving the cursor or scrolling.
 * No-op when out of bounds or without a framebuffer. */
void console_putc_at(uint32_t col, uint32_t row, char c);

// Writes a box-drawing glyph at the cursor or at a fixed cell.
void console_putc_box(enum nx_box_glyph g);
void console_putc_box_at(uint32_t col, uint32_t row, enum nx_box_glyph g);

// Suspends or resumes framebuffer console output without affecting serial logging.
void console_set_suspended(bool suspended);
bool console_is_suspended(void);

#endif /* NEXUS_CONSOLE_H */
