#include "video/fb.h"
#include "boot/requests.h"

static uint8_t  *fb_base;
static uint64_t  fb_w, fb_h, fb_pitch;
static uint32_t  fb_bpp;
static uint32_t  fb_bytes_per_px;
static uint8_t   r_size, r_shift, g_size, g_shift, b_size, b_shift;
static bool      fb_ok = false;

bool fb_available(void) {
    return fb_ok;
}

void fb_init(void) {
    struct limine_framebuffer *fb = g_boot.fb;
    if (fb == NULL) {
        fb_ok = false;
        return;
    }

    fb_base  = (uint8_t *)fb->address;
    fb_w     = fb->width;
    fb_h     = fb->height;
    fb_pitch = fb->pitch;
    fb_bpp   = fb->bpp;
    fb_bytes_per_px = (fb_bpp + 7) / 8;

    r_size = fb->red_mask_size;   r_shift = fb->red_mask_shift;
    g_size = fb->green_mask_size; g_shift = fb->green_mask_shift;
    b_size = fb->blue_mask_size;  b_shift = fb->blue_mask_shift;

    fb_ok = true;
}

uint64_t fb_width(void)  { return fb_w; }
uint64_t fb_height(void) { return fb_h; }

static inline uint32_t pack_rgb(uint32_t rgb) {
    uint8_t r = (rgb >> 16) & 0xFF;
    uint8_t g = (rgb >> 8) & 0xFF;
    uint8_t b = rgb & 0xFF;

    uint32_t rv = (r_size >= 8) ? r : (r >> (8 - r_size));
    uint32_t gv = (g_size >= 8) ? g : (g >> (8 - g_size));
    uint32_t bv = (b_size >= 8) ? b : (b >> (8 - b_size));

    return (rv << r_shift) | (gv << g_shift) | (bv << b_shift);
}

void fb_put_pixel(uint64_t x, uint64_t y, uint32_t rgb) {
    if (!fb_ok || x >= fb_w || y >= fb_h) {
        return;
    }
    uint32_t px = pack_rgb(rgb);
    uint8_t *dst = fb_base + y * fb_pitch + x * fb_bytes_per_px;
    for (uint32_t i = 0; i < fb_bytes_per_px; i++) {
        dst[i] = (uint8_t)(px >> (i * 8));
    }
}

void fb_fill_rect(uint64_t x, uint64_t y, uint64_t w, uint64_t h, uint32_t rgb) {
    if (!fb_ok) {
        return;
    }
    uint64_t x1 = MIN(x + w, fb_w);
    uint64_t y1 = MIN(y + h, fb_h);
    for (uint64_t yy = y; yy < y1; yy++) {
        for (uint64_t xx = x; xx < x1; xx++) {
            fb_put_pixel(xx, yy, rgb);
        }
    }
}

void fb_clear(uint32_t rgb) {
    fb_fill_rect(0, 0, fb_w, fb_h, rgb);
}

void fb_scroll_up(uint64_t rows, uint32_t bg) {
    if (!fb_ok || rows == 0 || rows >= fb_h) {
        fb_clear(bg);
        return;
    }
    uint64_t bytes_per_row = fb_pitch;
    uint8_t *dst = fb_base;
    uint8_t *src = fb_base + rows * fb_pitch;
    uint64_t move_rows = fb_h - rows;
    memmove(dst, src, move_rows * bytes_per_row);
    fb_fill_rect(0, move_rows, fb_w, rows, bg);
}
