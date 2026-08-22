#include "video/fb.h"
#include "boot/requests.h"

static uint8_t *fb_base;
static uint64_t fb_w, fb_h, fb_pitch;
static uint32_t fb_bpp;
static uint32_t fb_bytes_per_px;
static uint8_t r_size, r_shift, g_size, g_shift, b_size, b_shift;
static bool fb_ok = false;

bool fb_available(void) { return fb_ok; }

void fb_init(void) {
  struct limine_framebuffer *fb = g_boot.fb;
  if (fb == NULL) {
    fb_ok = false;
    return;
  }

  fb_base = (uint8_t *)fb->address;
  fb_w = fb->width;
  fb_h = fb->height;
  fb_pitch = fb->pitch;
  fb_bpp = fb->bpp;
  fb_bytes_per_px = (fb_bpp + 7) / 8;

  r_size = fb->red_mask_size;
  r_shift = fb->red_mask_shift;
  g_size = fb->green_mask_size;
  g_shift = fb->green_mask_shift;
  b_size = fb->blue_mask_size;
  b_shift = fb->blue_mask_shift;

  fb_ok = true;
}

uint64_t fb_width(void) { return fb_w; }
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

/* Inverse of pack_rgb() -- unpacks a raw hardware pixel value back
 * into 0x00RRGGBB, using the same red/green/blue mask size/shift
 * fb_init() recorded from Limine's framebuffer response. Only
 * fb_blend_pixel() needs this: every other primitive here is
 * write-only, since nothing before it ever needed to read a pixel
 * back. Lossy the same way pack_rgb() is lossy (a mode with fewer
 * than 8 bits per channel loses precision going in, and this widens
 * it back out via a left-shift rather than reconstructing bits that
 * were never stored) -- fine for blending translucent UI chrome, not
 * meant for anything that needs bit-exact colour round-tripping. */
static inline uint32_t unpack_rgb(uint32_t raw) {
  uint32_t r_mask = (1u << r_size) - 1u;
  uint32_t g_mask = (1u << g_size) - 1u;
  uint32_t b_mask = (1u << b_size) - 1u;

  uint32_t rv = (raw >> r_shift) & r_mask;
  uint32_t gv = (raw >> g_shift) & g_mask;
  uint32_t bv = (raw >> b_shift) & b_mask;

  uint8_t r = (uint8_t)((r_size >= 8) ? rv : (rv << (8 - r_size)));
  uint8_t g = (uint8_t)((g_size >= 8) ? gv : (gv << (8 - g_size)));
  uint8_t b = (uint8_t)((b_size >= 8) ? bv : (bv << (8 - b_size)));

  return ((uint32_t)r << 16) | ((uint32_t)g << 8) | b;
}

static inline uint32_t read_raw_pixel(uint64_t x, uint64_t y) {
  uint8_t *src = fb_base + y * fb_pitch + x * fb_bytes_per_px;
  uint32_t raw = 0;
  for (uint32_t i = 0; i < fb_bytes_per_px; i++) {
    raw |= (uint32_t)src[i] << (i * 8);
  }
  return raw;
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

void fb_blend_pixel(uint64_t x, uint64_t y, uint32_t rgb, uint8_t alpha) {
  if (!fb_ok || x >= fb_w || y >= fb_h || alpha == 0) {
    return;
  }
  if (alpha >= 255) {
    fb_put_pixel(x, y, rgb);
    return;
  }

  uint32_t dst_rgb = unpack_rgb(read_raw_pixel(x, y));

  uint8_t sr = (uint8_t)(rgb >> 16), sg = (uint8_t)(rgb >> 8),
          sb = (uint8_t)rgb;
  uint8_t dr = (uint8_t)(dst_rgb >> 16), dg = (uint8_t)(dst_rgb >> 8),
          db = (uint8_t)dst_rgb;

  /* Plain integer linear interpolation -- this build has no FPU
   * (-mno-80387/-mno-sse/-mno-mmx, see kernel/Makefile), so every
   * blend anywhere in this kernel has to stay fixed-point/integer. */
  uint8_t out_r =
      (uint8_t)(((uint32_t)sr * alpha + (uint32_t)dr * (255 - alpha)) / 255);
  uint8_t out_g =
      (uint8_t)(((uint32_t)sg * alpha + (uint32_t)dg * (255 - alpha)) / 255);
  uint8_t out_b =
      (uint8_t)(((uint32_t)sb * alpha + (uint32_t)db * (255 - alpha)) / 255);

  fb_put_pixel(x, y, ((uint32_t)out_r << 16) | ((uint32_t)out_g << 8) | out_b);
}

void fb_fill_rect(uint64_t x, uint64_t y, uint64_t w, uint64_t h,
                  uint32_t rgb) {
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

void fb_clear(uint32_t rgb) { fb_fill_rect(0, 0, fb_w, fb_h, rgb); }

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
