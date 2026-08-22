#include "video/splash.h"
#include "video/console.h"
#include "video/fb.h"
#include "video/nx_font8x8.h"

/* Full-screen boot splash, built directly on fb.h's pixel primitives
 * rather than console.h's fixed-8x8-cell text -- the wordmark below
 * is drawn several times larger than a normal console glyph, which
 * console_putc() has no concept of. See console_set_suspended()'s own
 * comment for how this coexists with kprintf()'s boot-log output
 * (still going to serial the whole time, just not to the screen). */

#define GLYPH_SRC 8

#define WORDMARK "NEXUS"
#define TAGLINE "x86-64  *  SMP  *  x2APIC"

#define COLOR_BAR_FILL NX_COLOR_ACCENT
#define COLOR_BAR_EMPTY 0x001A2230
#define COLOR_BAR_BORDER NX_COLOR_DIM

static bool splash_active = false;
static uint32_t bar_x, bar_y, bar_w, bar_h;
static uint32_t status_y;
static uint32_t status_scale;

/* Draws one glyph with each source pixel blown up into an `s`x`s`
 * block -- the only way to get a bigger-than-8px character out of
 * font8x8_basic without a second font asset. Cheap enough at boot
 * time (at most 64 fb_fill_rect() calls per glyph) that there's no
 * need for anything smarter. */
static void draw_glyph_scaled(uint32_t px0, uint32_t py0, char c, uint32_t s,
                              uint32_t color) {
  unsigned char uc = (unsigned char)c;
  if (uc >= 128) {
    uc = '?';
  }
  const unsigned char *glyph = (const unsigned char *)nx_font8x8[uc];

  for (int y = 0; y < GLYPH_SRC; y++) {
    unsigned char bits = glyph[y];
    for (int x = 0; x < GLYPH_SRC; x++) {
      if (bits & (1 << x)) {
        fb_fill_rect(px0 + (uint64_t)x * s, py0 + (uint64_t)y * s, s, s, color);
      }
    }
  }
}

/* Total on-screen width of `s` drawn at `glyph_scale` with `spacing`
 * pixels between glyphs (none trailing the last one) -- shared by the
 * centering math in draw_text_scaled() and by splash_show()'s own
 * search for the biggest wordmark scale that still fits. */
static uint32_t text_width(const char *s, uint32_t glyph_scale,
                           uint32_t spacing) {
  size_t len = strlen(s);
  if (len == 0) {
    return 0;
  }
  uint32_t cell = GLYPH_SRC * glyph_scale + spacing;
  return (uint32_t)(len * cell - spacing);
}

static void draw_text_scaled(uint32_t y, const char *s, uint32_t glyph_scale,
                             uint32_t spacing, uint32_t color, bool centered) {
  uint32_t cell = GLYPH_SRC * glyph_scale + spacing;
  uint32_t w = text_width(s, glyph_scale, spacing);
  uint64_t fbw = fb_width();
  uint32_t x = (centered && fbw > w) ? (uint32_t)((fbw - w) / 2) : 0;

  for (const char *p = s; *p; p++) {
    draw_glyph_scaled(x, y, *p, glyph_scale, color);
    x += cell;
  }
}

void splash_show(void) {
  if (!fb_available()) {
    return;
  }

  console_set_suspended(true);
  fb_clear(NX_COLOR_BG);

  uint64_t w = fb_width();
  uint64_t h = fb_height();

  /* Biggest wordmark scale that still leaves comfortable side margins
   * -- a fixed scale would either be illegibly small on a large panel
   * or overflow a small one (QEMU's default mode vs. a real display).
   * Falls back to 1 (plain console-sized glyphs) if nothing bigger
   * fits, rather than ever overflowing the screen. */
  uint32_t logo_scale = 1;
  for (uint32_t s = 12; s >= 2; s--) {
    if (text_width(WORDMARK, s, s / 2) <= (uint32_t)(w * 6 / 10)) {
      logo_scale = s;
      break;
    }
  }

  uint32_t logo_h = GLYPH_SRC * logo_scale;
  uint32_t logo_y = (uint32_t)(h * 34 / 100);
  draw_text_scaled(logo_y, WORDMARK, logo_scale, logo_scale / 2,
                   NX_COLOR_ACCENT, true);

  uint32_t tagline_scale = MAX(1u, logo_scale / 6);
  uint32_t tagline_y = logo_y + logo_h + 8 * tagline_scale;
  draw_text_scaled(tagline_y, TAGLINE, tagline_scale, tagline_scale,
                   NX_COLOR_DIM, true);

  bar_w = (uint32_t)(w * 4 / 10);
  bar_h = 14;
  bar_x = (uint32_t)((w - bar_w) / 2);
  bar_y = (uint32_t)(h * 60 / 100);
  fb_fill_rect(bar_x - 2, bar_y - 2, bar_w + 4, bar_h + 4, COLOR_BAR_BORDER);
  fb_fill_rect(bar_x, bar_y, bar_w, bar_h, COLOR_BAR_EMPTY);

  status_y = bar_y + bar_h + 14;
  status_scale = 1;

  splash_active = true;
  splash_progress(0, "starting up...");
}

void splash_progress(uint32_t percent, const char *label) {
  if (!splash_active) {
    return;
  }
  if (percent > 100) {
    percent = 100;
  }

  uint32_t fill_w = (uint32_t)((uint64_t)bar_w * percent / 100);
  fb_fill_rect(bar_x, bar_y, bar_w, bar_h, COLOR_BAR_EMPTY);
  if (fill_w > 0) {
    fb_fill_rect(bar_x, bar_y, fill_w, bar_h, COLOR_BAR_FILL);
  }

  /* Status line: blank a fixed-height band across the full width
   * first (simpler and cheaper than measuring the previous label's
   * exact extent), then draw the new one centered. */
  fb_fill_rect(0, status_y, fb_width(), GLYPH_SRC * status_scale + 4,
               NX_COLOR_BG);
  draw_text_scaled(status_y, label, status_scale, status_scale, NX_COLOR_FG,
                   true);
}

void splash_finish(void) {
  if (!splash_active) {
    return;
  }
  splash_active = false;
  fb_clear(NX_COLOR_BG);
  console_set_suspended(false);
  console_clear();
}
