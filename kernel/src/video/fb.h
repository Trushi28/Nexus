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
void fb_fill_rect(uint64_t x, uint64_t y, uint64_t w, uint64_t h, uint32_t rgb);
void fb_clear(uint32_t rgb);

/* Fast whole-framebuffer scroll-up by `rows` pixel rows, filling the
 * newly exposed bottom strip with `bg`. */
void fb_scroll_up(uint64_t rows, uint32_t bg);

#endif /* NEXUS_FB_H */
