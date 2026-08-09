#ifndef NEXUS_CONSOLE_H
#define NEXUS_CONSOLE_H

#include "klib/klib.h"

void console_init(void);
void console_clear(void);
void console_putc(char c);
void console_puts(const char *s);
void console_set_colors(uint32_t fg, uint32_t bg);

/* Handles backspace by erasing the previous glyph cell, used by the
 * shell's line editor. No-op at column 0 of a fresh line. */
void console_backspace(void);

/* The console's size in character cells (not pixels). Both read 0 if
 * there's no framebuffer. */
uint32_t console_cols(void);
uint32_t console_rows(void);

/* Draws a single glyph at an arbitrary (col, row) cell without moving
 * the cursor or triggering a scroll -- unlike console_putc(), which
 * always writes at the cursor and advances it. For free-form text
 * placement (the `matrix` shell command uses this to plot falling
 * characters at random columns). No-op if (col, row) is out of range
 * or there's no framebuffer. */
void console_putc_at(uint32_t col, uint32_t row, char c);

#endif /* NEXUS_CONSOLE_H */
