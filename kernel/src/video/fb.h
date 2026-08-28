#ifndef NEXUS_FB_H
#define NEXUS_FB_H

#include "klib/klib.h"

// True when framebuffer output is available.
bool fb_available(void);

void fb_init(void);

uint64_t fb_width(void);
uint64_t fb_height(void);

void fb_put_pixel(uint64_t x, uint64_t y, uint32_t rgb);

/* Alpha-blends `rgb` onto the pixel at (x, y).
 * alpha: 0 = transparent, 255 = opaque. */
void fb_blend_pixel(uint64_t x, uint64_t y, uint32_t rgb, uint8_t alpha);

void fb_fill_rect(uint64_t x, uint64_t y, uint64_t w, uint64_t h, uint32_t rgb);
void fb_clear(uint32_t rgb);

/* Fast whole-framebuffer scroll-up by `rows` pixel rows, filling the
 * newly exposed bottom strip with `bg`. */
void fb_scroll_up(uint64_t rows, uint32_t bg);

#endif /* NEXUS_FB_H */
