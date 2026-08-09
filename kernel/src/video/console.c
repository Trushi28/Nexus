#include "video/console.h"
#include "video/fb.h"
#include <font8x8_basic.h>

/* font8x8_basic.h (dhepper/font8x8, Public Domain) defines:
 *     static const unsigned char font8x8_basic[128][8];
 * One byte per row, bit `x` (1 << x) set means "pixel on" at column x. */

#define GLYPH_W 8
#define GLYPH_H 8
#define CELL_W  GLYPH_W
#define CELL_H  (GLYPH_H + 2) /* a little vertical breathing room */

static uint32_t cols, rows;
static uint32_t cur_col, cur_row;
static uint32_t fg_color = 0x00E0E0E0;
static uint32_t bg_color = 0x000B0E14;

void console_set_colors(uint32_t fg, uint32_t bg) {
    fg_color = fg;
    bg_color = bg;
}

void console_init(void) {
    if (!fb_available()) {
        return;
    }
    cols = (uint32_t)(fb_width() / CELL_W);
    rows = (uint32_t)(fb_height() / CELL_H);
    cur_col = 0;
    cur_row = 0;
    fb_clear(bg_color);
}

void console_clear(void) {
    cur_col = 0;
    cur_row = 0;
    fb_clear(bg_color);
}

static void draw_glyph(uint32_t col, uint32_t row, char c) {
    uint64_t px0 = (uint64_t)col * CELL_W;
    uint64_t py0 = (uint64_t)row * CELL_H;

    fb_fill_rect(px0, py0, CELL_W, CELL_H, bg_color);

    unsigned char uc = (unsigned char)c;
    if (uc >= 128) {
        uc = '?';
    }
    const unsigned char *glyph = (const unsigned char *)font8x8_basic[uc];

    for (int y = 0; y < GLYPH_H; y++) {
        unsigned char bits = glyph[y];
        for (int x = 0; x < GLYPH_W; x++) {
            if (bits & (1 << x)) {
                fb_put_pixel(px0 + x, py0 + y, fg_color);
            }
        }
    }
}

static void newline(void) {
    cur_col = 0;
    if (cur_row + 1 >= rows) {
        fb_scroll_up(CELL_H, bg_color);
    } else {
        cur_row++;
    }
}

void console_putc(char c) {
    if (!fb_available()) {
        return;
    }

    if (c == '\n') {
        newline();
        return;
    }
    if (c == '\r') {
        cur_col = 0;
        return;
    }
    if (c == '\t') {
        uint32_t next = (cur_col / 4 + 1) * 4;
        while (cur_col < next && cur_col < cols) {
            console_putc(' ');
        }
        return;
    }
    if (c == '\b') {
        console_backspace();
        return;
    }

    draw_glyph(cur_col, cur_row, c);
    cur_col++;
    if (cur_col >= cols) {
        newline();
    }
}

void console_backspace(void) {
    if (!fb_available()) {
        return;
    }
    if (cur_col == 0) {
        return;
    }
    cur_col--;
    fb_fill_rect((uint64_t)cur_col * CELL_W, (uint64_t)cur_row * CELL_H,
                 CELL_W, CELL_H, bg_color);
}

uint32_t console_cols(void) { return cols; }
uint32_t console_rows(void) { return rows; }

void console_putc_at(uint32_t col, uint32_t row, char c) {
    if (!fb_available() || col >= cols || row >= rows) {
        return;
    }
    draw_glyph(col, row, c);
}

void console_puts(const char *s) {
    while (*s) {
        console_putc(*s++);
    }
}
