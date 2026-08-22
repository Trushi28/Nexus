#ifndef NEXUS_FB_H
#define NEXUS_FB_H

#include "klib/klib.h"

/* True if a Limine framebuffer was found and fb_init() succeeded. Video
 * output is optional -- the kernel is fully usable over serial alone. */
bool fb_available(void);

void fb_init(void);

uint64_t fb_width(void);
uint64_t fb_height(void);

void fb_put_pixel(uint64_t x, uint64_t y, uint32_t rgb);

/* Alpha-blends `rgb` onto the pixel at (x,y) with weight `alpha`
 * (0 = fully transparent/no-op, 255 = fully opaque/same as
 * fb_put_pixel()) instead of overwriting it outright. A genuine
 * read-modify-write: it reads back whatever's actually at (x,y) right
 * now (unpacked through the same red/green/blue mask/shift layout
 * fb_init() recorded) and mixes toward `rgb`, so layering several
 * blended draws over each other composites the way you'd expect.
 * video/splash.c uses this for glow/gradient/soft-edge effects. A
 * no-op out of bounds or with no framebuffer, same as fb_put_pixel(). */
void fb_blend_pixel(uint64_t x, uint64_t y, uint32_t rgb, uint8_t alpha);

void fb_fill_rect(uint64_t x, uint64_t y, uint64_t w, uint64_t h, uint32_t rgb);
void fb_clear(uint32_t rgb);

/* Fast whole-framebuffer scroll-up by `rows` pixel rows, filling the
 * newly exposed bottom strip with `bg`. */
void fb_scroll_up(uint64_t rows, uint32_t bg);

#endif /* NEXUS_FB_H */
