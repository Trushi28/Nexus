#include "video/splash.h"
#include "time/pit.h"
#include "video/console.h"
#include "video/fb.h"
#include "video/nx_font8x8.h"

/*
 * Full-screen boot splash rendered directly through framebuffer primitives.
 * Console output is suspended while the splash owns the framebuffer.
 *
 * Animation timing uses pit_wait_ms() because the normal kernel timer is
 * not active this early in boot.
 */

#define GLYPH_SRC 8

#define WORDMARK "NEXUS"
#define TAGLINE "x86-64  *  SMP  *  x2APIC"

#define COLOR_BAR_FILL NX_COLOR_ACCENT
#define COLOR_BAR_EMPTY 0x001A2230
#define COLOR_BAR_BORDER NX_COLOR_DIM
#define COLOR_PANEL 0x00152030

// Progress transition timing.
#define ANIM_FRAMES 14
#define ANIM_FRAME_MS 12

// Per-character reveal delays.
#define ANIM_WORDMARK_LETTER_MS 45
#define ANIM_TAGLINE_LETTER_MS 8

#define ANIM_WIPE_STEPS 24

static bool splash_active = false;
static uint32_t bar_x, bar_y, bar_w, bar_h;
static uint32_t status_y;
static uint32_t status_scale;
static uint32_t status_band_color;
static uint32_t panel_top, panel_bottom;

// Animation state preserved across splash_progress() calls.
static uint32_t anim_fill_w = 0;
static uint32_t anim_tick = 0;

// Fixed-point RGB interpolation. `alpha` is the weight of `b`.
static uint32_t mix_rgb(uint32_t a, uint32_t b, uint32_t alpha) {
  uint8_t ar = (uint8_t)(a >> 16), ag = (uint8_t)(a >> 8), ab = (uint8_t)a;
  uint8_t br = (uint8_t)(b >> 16), bg = (uint8_t)(b >> 8), bb = (uint8_t)b;

  uint8_t r = (uint8_t)((br * alpha + ar * (255 - alpha)) / 255);
  uint8_t g = (uint8_t)((bg * alpha + ag * (255 - alpha)) / 255);
  uint8_t bch = (uint8_t)((bb * alpha + ab * (255 - alpha)) / 255);

  return ((uint32_t)r << 16) | ((uint32_t)g << 8) | bch;
}

// Returns a triangular alpha falloff centered on the content band.
static uint8_t panel_alpha_at(uint32_t y) {
  if (y < panel_top || y >= panel_bottom) {
    return 0;
  }
  uint32_t half = (panel_bottom - panel_top) / 2;
  if (half == 0) {
    return 0;
  }
  uint32_t center = panel_top + half;
  uint32_t dy = (y > center) ? (y - center) : (center - y);
  if (dy >= half) {
    return 0;
  }
  return (uint8_t)(70 - (70 * dy) / half);
}

/* Draws a softened square with blended edges.
 * Falls back to a solid fill for very small sizes. */
static void fill_soft_square(uint32_t x0, uint32_t y0, uint32_t s, uint32_t color) {
  if (s <= 2) {
    fb_fill_rect(x0, y0, s, s, color);
    return;
  }

  fb_fill_rect(x0 + 1, y0 + 1, s - 2, s - 2, color);

  for (uint32_t x = 1; x < s - 1; x++) {
    fb_blend_pixel(x0 + x, y0, color, 190);
    fb_blend_pixel(x0 + x, y0 + s - 1, color, 190);
  }
  for (uint32_t y = 1; y < s - 1; y++) {
    fb_blend_pixel(x0, y0 + y, color, 190);
    fb_blend_pixel(x0 + s - 1, y0 + y, color, 190);
  }

  fb_blend_pixel(x0, y0, color, 90);
  fb_blend_pixel(x0 + s - 1, y0, color, 90);
  fb_blend_pixel(x0, y0 + s - 1, color, 90);
  fb_blend_pixel(x0 + s - 1, y0 + s - 1, color, 90);
}

static void draw_glyph_scaled(uint32_t px0, uint32_t py0, char c, uint32_t s, uint32_t color) {
  unsigned char uc = (unsigned char)c;
  if (uc >= 128) {
    uc = '?';
  }
  const uint8_t *glyph = nx_font8x8[uc];

  for (int y = 0; y < GLYPH_SRC; y++) {
    uint8_t bits = glyph[y];
    for (int x = 0; x < GLYPH_SRC; x++) {
      if (bits & (1 << x)) {
        fill_soft_square(px0 + (uint64_t)x * s, py0 + (uint64_t)y * s, s, color);
      }
    }
  }
}

static uint32_t text_width(const char *s, uint32_t glyph_scale, uint32_t spacing) {
  size_t len = strlen(s);
  if (len == 0) {
    return 0;
  }
  uint32_t cell = GLYPH_SRC * glyph_scale + spacing;
  return (uint32_t)(len * cell - spacing);
}

// Draws scaled text, optionally delaying between glyphs.
static void draw_text_scaled_animated(uint32_t y, const char *s, uint32_t glyph_scale, uint32_t spacing, uint32_t color, bool centered, uint32_t delay_ms) {
  uint32_t cell = GLYPH_SRC * glyph_scale + spacing;
  uint32_t w = text_width(s, glyph_scale, spacing);
  uint64_t fbw = fb_width();
  uint32_t x = (centered && fbw > w) ? (uint32_t)((fbw - w) / 2) : 0;

  for (const char *p = s; *p; p++) {
    draw_glyph_scaled(x, y, *p, glyph_scale, color);
    x += cell;
    if (delay_ms > 0) {
      pit_wait_ms(delay_ms);
    }
  }
}

// Draws a layered glow behind the wordmark.
static void draw_glow(uint32_t cx, uint32_t cy, uint32_t base_w, uint32_t base_h) {
  static const struct {
    uint32_t pad;
    uint8_t alpha;
  } layers[] = {
      {36, 10},
      {22, 14},
      {10, 20},
  };
  uint64_t fbw = fb_width(), fbh = fb_height();

  for (size_t i = 0; i < ARRAY_LEN(layers); i++) {
    uint32_t gw = base_w + layers[i].pad * 2;
    uint32_t gh = base_h + layers[i].pad * 2;
    uint32_t gx = (cx > gw / 2) ? cx - gw / 2 : 0;
    uint32_t gy = (cy > gh / 2) ? cy - gh / 2 : 0;
    uint32_t gx1 = (uint32_t)MIN((uint64_t)(gx + gw), fbw);
    uint32_t gy1 = (uint32_t)MIN((uint64_t)(gy + gh), fbh);

    for (uint32_t y = gy; y < gy1; y++) {
      for (uint32_t x = gx; x < gx1; x++) {
        fb_blend_pixel(x, y, NX_COLOR_ACCENT, layers[i].alpha);
      }
    }
  }
}

// Draws a deterministic scatter of faint background dots.
static void draw_dust(void) {
  uint64_t w = fb_width(), h = fb_height();
  uint32_t rng = 0x9E3779B9u;

  uint32_t dot_count = (uint32_t)((w * h) / 9000);
  if (dot_count > 400) {
    dot_count = 400;
  }

  for (uint32_t i = 0; i < dot_count; i++) {
    rng = rng * 1103515245u + 12345u;
    uint32_t dx = rng % (uint32_t)w;
    rng = rng * 1103515245u + 12345u;
    uint32_t dy = rng % (uint32_t)h;
    rng = rng * 1103515245u + 12345u;
    uint8_t alpha = (uint8_t)(16 + (rng % 26));
    fb_blend_pixel(dx, dy, NX_COLOR_DIM, alpha);
  }
}

// Draws one progress-bar animation frame.
static void draw_bar_frame(uint32_t fill_w, uint32_t frame) {
  fb_fill_rect(bar_x, bar_y, bar_w, bar_h, COLOR_BAR_EMPTY);

  if (fill_w == 0) {
    return;
  }
  if (fill_w > bar_w) {
    fill_w = bar_w;
  }

  fb_fill_rect(bar_x, bar_y, fill_w, bar_h, COLOR_BAR_FILL);

  /* A glassy highlight across the top third of the filled portion. */
  uint32_t sheen_h = bar_h / 3;
  for (uint32_t y = 0; y < sheen_h; y++) {
    for (uint32_t x = 0; x < fill_w; x++) {
      fb_blend_pixel(bar_x + x, bar_y + y, 0x00FFFFFF, 35);
    }
  }

  /* Moving shimmer across the filled portion. */
  uint32_t shimmer_w = 8;
  uint32_t span = fill_w + shimmer_w * 2;
  uint32_t phase = ((anim_tick * ANIM_FRAMES + frame) * 5) % span;
  int64_t shimmer_x0 = (int64_t)bar_x - (int64_t)shimmer_w + (int64_t)phase;

  for (uint32_t dx = 0; dx < shimmer_w; dx++) {
    int64_t x = shimmer_x0 + (int64_t)dx;
    if (x < (int64_t)bar_x || x >= (int64_t)(bar_x + fill_w)) {
      continue;
    }
    uint32_t half = shimmer_w / 2;
    uint32_t d = (dx <= half) ? dx : (shimmer_w - dx);
    uint8_t alpha = (uint8_t)(70 * d / half);
    for (uint32_t y = 0; y < bar_h; y++) {
      fb_blend_pixel((uint64_t)x, bar_y + y, 0x00FFFFFF, alpha);
    }
  }

  for (uint32_t i = 0; i < 3 && fill_w + i < bar_w; i++) {
    uint8_t edge_alpha = (uint8_t)(170 - i * 60);
    for (uint32_t y = 0; y < bar_h; y++) {
      fb_blend_pixel(bar_x + fill_w + i, bar_y + y, COLOR_BAR_FILL, edge_alpha);
    }
  }
}

/* Reveals characters from left to right while keeping the full label
 * position fixed. */
static void draw_status_partial(const char *label, uint32_t reveal_chars) {
  fb_fill_rect(0, status_y, fb_width(), GLYPH_SRC * status_scale + 4, status_band_color);

  uint32_t cell = GLYPH_SRC * status_scale + status_scale;
  uint32_t full_w = text_width(label, status_scale, status_scale);
  uint64_t fbw = fb_width();
  uint32_t x = (fbw > full_w) ? (uint32_t)((fbw - full_w) / 2) : 0;

  uint32_t shown = 0;
  for (const char *p = label; *p != '\0' && shown < reveal_chars; p++, shown++) {
    draw_glyph_scaled(x, status_y, *p, status_scale, NX_COLOR_FG);
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

  // Compute layout before drawing the content band.
  uint32_t logo_scale = 1;
  for (uint32_t s = 12; s >= 2; s--) {
    if (text_width(WORDMARK, s, s / 2) <= (uint32_t)(w * 6 / 10)) {
      logo_scale = s;
      break;
    }
  }
  uint32_t logo_h = GLYPH_SRC * logo_scale;
  uint32_t logo_y = (uint32_t)(h * 34 / 100);

  uint32_t tagline_scale = MAX(1u, logo_scale / 6);
  uint32_t tagline_y = logo_y + logo_h + 8 * tagline_scale;

  bar_w = (uint32_t)(w * 4 / 10);
  bar_h = 14;
  bar_x = (uint32_t)((w - bar_w) / 2);
  bar_y = (uint32_t)(h * 60 / 100);

  status_y = bar_y + bar_h + 14;
  status_scale = 1;

  panel_top = (logo_y > 70) ? logo_y - 70 : 0;
  panel_bottom = (uint32_t)MIN((uint64_t)(status_y + 60), h);

  /* --- draw, back to front --- */

  // Draw back to front.
  for (uint32_t y = panel_top; y < panel_bottom; y++) {
    uint32_t row_color = mix_rgb(NX_COLOR_BG, COLOR_PANEL, panel_alpha_at(y));
    fb_fill_rect(0, y, w, 1, row_color);
  }

  draw_dust();

  uint32_t wordmark_w = text_width(WORDMARK, logo_scale, logo_scale / 2);
  draw_glow((uint32_t)(w / 2), logo_y + logo_h / 2, wordmark_w, logo_h);

  draw_text_scaled_animated(logo_y, WORDMARK, logo_scale, logo_scale / 2, NX_COLOR_ACCENT, true, ANIM_WORDMARK_LETTER_MS);
  draw_text_scaled_animated(tagline_y, TAGLINE, tagline_scale, tagline_scale, NX_COLOR_DIM, true, ANIM_TAGLINE_LETTER_MS);

  fb_fill_rect(bar_x - 2, bar_y - 2, bar_w + 4, bar_h + 4, COLOR_BAR_BORDER);
  fb_fill_rect(bar_x, bar_y, bar_w, bar_h, COLOR_BAR_EMPTY);

  uint32_t fx0 = bar_x - 2, fy0 = bar_y - 2;
  uint32_t fx1 = bar_x + bar_w + 1, fy1 = bar_y + bar_h + 1;
  uint32_t top_bg = mix_rgb(NX_COLOR_BG, COLOR_PANEL, panel_alpha_at(fy0));
  uint32_t bot_bg = mix_rgb(NX_COLOR_BG, COLOR_PANEL, panel_alpha_at(fy1));
  fb_blend_pixel(fx0, fy0, top_bg, 200);
  fb_blend_pixel(fx1, fy0, top_bg, 200);
  fb_blend_pixel(fx0, fy1, bot_bg, 200);
  fb_blend_pixel(fx1, fy1, bot_bg, 200);

  status_band_color = mix_rgb(NX_COLOR_BG, COLOR_PANEL, panel_alpha_at(status_y));

  anim_fill_w = 0;
  anim_tick = 0;

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

  uint32_t target_w = (uint32_t)((uint64_t)bar_w * percent / 100);
  uint32_t start_w = anim_fill_w;
  size_t label_len = strlen(label);

  // Ease the bar toward its target while revealing the status label.
  for (uint32_t frame = 1; frame <= ANIM_FRAMES; frame++) {
    uint32_t rem = ANIM_FRAMES - frame;
    uint32_t frac_num = ANIM_FRAMES * ANIM_FRAMES - rem * rem;
    uint32_t frac_den = ANIM_FRAMES * ANIM_FRAMES;

    uint32_t interp_w;
    if (target_w >= start_w) {
      interp_w = start_w + (uint32_t)(((uint64_t)(target_w - start_w) * frac_num) / frac_den);
    } else {
      interp_w = start_w - (uint32_t)(((uint64_t)(start_w - target_w) * frac_num) / frac_den);
    }

    draw_bar_frame(interp_w, frame);

    uint32_t reveal = (uint32_t)(((uint64_t)label_len * frame + ANIM_FRAMES - 1) / ANIM_FRAMES);
    draw_status_partial(label, reveal);

    pit_wait_ms(ANIM_FRAME_MS);
  }

  anim_fill_w = target_w; // Preserve the exact final state.
  anim_tick++;
}

void splash_finish(void) {
  if (!splash_active) {
    return;
  }
  splash_active = false;

  uint64_t w = fb_width(), h = fb_height();
  uint64_t step_w = DIV_ROUND_UP(w, (uint64_t)ANIM_WIPE_STEPS);

  for (uint32_t i = 0; i < ANIM_WIPE_STEPS; i++) {
    uint64_t x0 = (uint64_t)i * step_w;
    if (x0 >= w) {
      break;
    }
    uint64_t cw = MIN(step_w, w - x0);
    fb_fill_rect(x0, 0, cw, h, NX_COLOR_BG);

    uint64_t edge_x = x0 + cw;
    if (edge_x < w) {
      fb_fill_rect(edge_x, 0, MIN((uint64_t)3, w - edge_x), h, NX_COLOR_ACCENT);
    }
    pit_wait_ms(ANIM_FRAME_MS);
  }

  fb_clear(NX_COLOR_BG);
  console_set_suspended(false);
  console_clear();
}
