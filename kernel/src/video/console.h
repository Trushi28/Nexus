#ifndef NEXUS_CONSOLE_H
#define NEXUS_CONSOLE_H

#include "klib/klib.h"
#include "video/nx_box8x8.h"

/* Shared 0x00RRGGBB color constants for kernel-side UI chrome (boot
 * splash, shell prompt/box-drawing, ...) built on top of
 * console_set_colors()/fb_fill_rect() -- kept in one place so every
 * caller agrees on what "the accent color" actually is, instead of
 * the same hex literals getting hand-copied into each file. */
#define NX_COLOR_BG 0x000B0E14
#define NX_COLOR_FG 0x00E0E0E0
#define NX_COLOR_ACCENT 0x0057C7FF
#define NX_COLOR_DIM 0x00445566
#define NX_COLOR_ERROR 0x00FF6B6B

void console_init(void);
void console_clear(void);

/* Writes one raw byte at the cursor -- always ASCII, never UTF-8
 * decoded. Every direct caller (the shell's line editor echoing a
 * keystroke, console_puts()'s own decode loop below, console_putc_box())
 * only ever hands this one already-known single byte at a time, so
 * there's nothing here that needs multi-byte awareness. */
void console_putc(char c);

/* Writes a full string at the cursor, advancing/wrapping/scrolling as
 * needed -- and, unlike console_putc(), UTF-8 aware: a multi-byte
 * sequence matching one of nx_box8x8.h's 22 box-drawing codepoints
 * renders as that glyph, and anything else undecodable renders as a
 * single '?' (never several, one per raw byte) -- see klib/utf8.h and
 * this function's own comment in console.c for the full story. Plain
 * ASCII is unaffected either way. */
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
 * or there's no framebuffer. Not affected by console_set_suspended()
 * -- callers that draw this way are already managing the screen
 * themselves. */
void console_putc_at(uint32_t col, uint32_t row, char c);

/* Box-drawing counterparts to console_putc()/console_putc_at() -- see
 * video/nx_box8x8.h for exactly which 22 glyphs. Separate,
 * explicitly-named entry points rather than something a caller reaches
 * by encoding the actual UTF-8 bytes and going through console_puts()
 * -- shell/shell.c's box-drawing helpers (print_box_border(),
 * print_divider(), ...) are building fixed UI chrome from a known
 * enum value, not printing arbitrary text that might happen to
 * contain one of these, so there's no byte-stream decoding to pay for
 * on that path. console_puts() (above) is the one that DOES decode
 * UTF-8 and can also reach these same 22 glyphs, for arbitrary text
 * that contains them.
 * Same cell size, same suspended/bounds behavior as their console_putc*
 * equivalents. */
void console_putc_box(enum nx_box_glyph g);
void console_putc_box_at(uint32_t col, uint32_t row, enum nx_box_glyph g);

/* Suspends (or resumes) console_putc()/console_puts()/
 * console_backspace() actually drawing anything -- kprintf() itself
 * still always writes to serial regardless (see debug/log.c), so log
 * output is never lost, it just stops scribbling over whatever else
 * currently owns the framebuffer. Cursor position is left untouched
 * while suspended, so resuming picks up exactly where text output
 * left off. Used by video/splash.c to keep the boot log flowing to
 * serial while the splash screen owns the framebuffer directly. */
void console_set_suspended(bool suspended);
bool console_is_suspended(void);

#endif /* NEXUS_CONSOLE_H */
