#include "video/splash.h"
#include "time/pit.h"
#include "video/console.h"
#include "video/fb.h"
#include "video/nx_font8x8.h"

/* Full-screen boot splash, built directly on fb.h's pixel primitives
 * rather than console.h's fixed-8x8-cell text -- the wordmark below
 * is drawn several times larger than a normal console glyph, which
 * console_putc() has no concept of. See console_set_suspended()'s own
 * comment for how this coexists with kprintf()'s boot-log output
 * (still going to serial the whole time, just not to the screen).
 *
 * Every visual effect below (the gradient panel, the wordmark's glow,
 * the softened glyph edges, the progress bar's sheen) is built out of
 * fb_blend_pixel()'s plain integer alpha blend -- this build has no
 * FPU at all (-mno-80387/-mno-sse/-mno-mmx), so there's no float-based
 * antialiasing or radial-gradient math anywhere here, just fixed-point
 * mixing and rectangles. See fb.c's fb_blend_pixel()/unpack_rgb() for
 * the actual read-modify-write this all sits on top of.
 *
 * --------------------------------- motion ---------------------------------
 * Everything used to snap straight to its final state -- the bar jumped to
 * its new width, the status label just appeared, splash_finish() cut
 * straight to a blank screen. Every one of those transitions is now played
 * out over a short burst of discrete frames instead (see the ANIM_* knobs
 * below): the bar eases toward its new width with a moving shimmer riding
 * over it, the status label types itself in, the wordmark/tagline reveal
 * letter by letter, and splash_finish() wipes the screen clear instead of
 * blanking it outright. Frame pacing uses pit_wait_ms(), NOT
 * timer_busy_wait_ms() -- the LAPIC timer isn't ticking yet this early in
 * boot (interrupts are still globally off; nothing calls sti() until the
 * scheduler starts, see sched_enter_idle() in sched/sched.c), so
 * timer_uptime_ms() would sit at 0 forever and a busy-wait against it would
 * never return. pit_wait_ms() polls the raw 8253 PIT directly instead --
 * the exact same primitive timer_calibrate() already uses this early --
 * capped at ~54ms per call, which every delay below stays well under. */

#define GLYPH_SRC 8

#define WORDMARK "NEXUS"
#define TAGLINE "x86-64  *  SMP  *  x2APIC"

#define COLOR_BAR_FILL NX_COLOR_ACCENT
#define COLOR_BAR_EMPTY 0x001A2230
#define COLOR_BAR_BORDER NX_COLOR_DIM
#define COLOR_PANEL                                                            \
  0x00152030 /* subtle lighter tint behind the content band                    \
              */

/* Frames per splash_progress() transition, and the pit_wait_ms() budget
 * per frame -- ANIM_FRAMES * ANIM_FRAME_MS is roughly how long one call
 * takes to settle (~170ms), and main.c calls splash_progress() about 9
 * times over the whole boot, so the added wall-clock cost of all this
 * motion stays under two seconds total. */
#define ANIM_FRAMES 14
#define ANIM_FRAME_MS 12

/* Per-letter delay for the wordmark/tagline reveal in splash_show() --
 * the wordmark gets a deliberately dramatic pace, the tagline a quick
 * one, so the wordmark reads as the "main event". */
#define ANIM_WORDMARK_LETTER_MS 45
#define ANIM_TAGLINE_LETTER_MS 8

/* How many vertical strips splash_finish()'s wipe-out sweeps through. */
#define ANIM_WIPE_STEPS 24

static bool splash_active = false;
static uint32_t bar_x, bar_y, bar_w, bar_h;
static uint32_t status_y;
static uint32_t status_scale;
static uint32_t status_band_color;
static uint32_t panel_top, panel_bottom;

/* Animation state, carried across splash_progress() calls so a
 * transition always eases FROM wherever the bar actually is, not from
 * some assumed starting point, and so the shimmer sweep keeps moving
 * across separate calls instead of resetting to the same spot every
 * time. Both reset in splash_show(). */
static uint32_t anim_fill_w = 0;
static uint32_t anim_tick = 0;

/* --------------------------- integer color math --------------------------
 * `alpha` is a 0-255 weight of `b`. Plain fixed-point lerp -- the same
 * shape as fb_blend_pixel()'s own math, just operating on two logical
 * 0x00RRGGBB colors instead of one logical color and a framebuffer
 * readback, so a caller that already knows both endpoints (like the
 * background gradient below, painted onto a screen that was JUST
 * cleared to a known color) doesn't need to pay for a read-modify-
 * write it doesn't need. */
static uint32_t mix_rgb(uint32_t a, uint32_t b, uint32_t alpha) {
  uint8_t ar = (uint8_t)(a >> 16), ag = (uint8_t)(a >> 8), ab = (uint8_t)a;
  uint8_t br = (uint8_t)(b >> 16), bg = (uint8_t)(b >> 8), bb = (uint8_t)b;

  uint8_t r = (uint8_t)((br * alpha + ar * (255 - alpha)) / 255);
  uint8_t g = (uint8_t)((bg * alpha + ag * (255 - alpha)) / 255);
  uint8_t bch = (uint8_t)((bb * alpha + ab * (255 - alpha)) / 255);

  return ((uint32_t)r << 16) | ((uint32_t)g << 8) | bch;
}

/* Triangular (linear) falloff around the vertical center of
 * [panel_top, panel_bottom) -- the closest thing to a soft radial
 * vignette this gets without sqrt(). Peaks at 70/255 right at the
 * center row, 0 at and beyond the band's edges. */
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

/* Draws a glyph-cell-sized filled square with its outer 1px ring (and
 * especially its 4 corners) blended at reduced alpha instead of drawn
 * solid -- the closest thing to antialiasing a plain solid-fill
 * framebuffer with no supersampling gets: a soft, slightly rounded
 * edge instead of a hard-aliased one, at a fixed, cheap, always-on
 * cost (a constant number of extra blend calls per square, not
 * proportional to rendering at a higher resolution and downsampling).
 * Falls back to a plain solid fb_fill_rect() below 3px, where there's
 * no room for a separate core + ring. */
static void fill_soft_square(uint32_t x0, uint32_t y0, uint32_t s,
                             uint32_t color) {
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

static void draw_glyph_scaled(uint32_t px0, uint32_t py0, char c, uint32_t s,
                              uint32_t color) {
  unsigned char uc = (unsigned char)c;
  if (uc >= 128) {
    uc = '?';
  }
  const uint8_t *glyph = nx_font8x8[uc];

  for (int y = 0; y < GLYPH_SRC; y++) {
    uint8_t bits = glyph[y];
    for (int x = 0; x < GLYPH_SRC; x++) {
      if (bits & (1 << x)) {
        fill_soft_square(px0 + (uint64_t)x * s, py0 + (uint64_t)y * s, s,
                         color);
      }
    }
  }
}

static uint32_t text_width(const char *s, uint32_t glyph_scale,
                           uint32_t spacing) {
  size_t len = strlen(s);
  if (len == 0) {
    return 0;
  }
  uint32_t cell = GLYPH_SRC * glyph_scale + spacing;
  return (uint32_t)(len * cell - spacing);
}

/* Draws `s` scaled up `glyph_scale`x, one glyph at a time, pausing
 * `delay_ms` between each -- pass 0 for an ordinary instant blit, or
 * a real delay for a typewriter-style entrance. Used with a delay for
 * the wordmark/tagline in splash_show(): both are drawn on top of the
 * glow draw_glow() already painted, so each letter seems to emerge
 * out of it rather than materializing on a blank background. */
static void draw_text_scaled_animated(uint32_t y, const char *s,
                                      uint32_t glyph_scale, uint32_t spacing,
                                      uint32_t color, bool centered,
                                      uint32_t delay_ms) {
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

/* A handful of large, faint, concentric rectangles behind the
 * wordmark, each a little bigger and a little fainter than the last
 * -- a cheap manual approximation of a soft glow/bloom, entirely via
 * fb_blend_pixel() (no real blur kernel, no floats). */
static void draw_glow(uint32_t cx, uint32_t cy, uint32_t base_w,
                      uint32_t base_h) {
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

/* Fixed, deterministic scatter of very faint dots across the whole
 * screen -- a splash screen looking identical every single boot is a
 * feature here, not a limitation, so this is seeded from a constant,
 * not from anything time- or hardware-derived. Same LCG shape as the
 * `matrix` shell command (shell/shell.c) uses for its own rain. */
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

/* Renders ONE animation frame of the progress bar at `fill_w` pixels
 * filled -- called several times per splash_progress() transition
 * (see ANIM_FRAMES) so the bar visibly grows/shrinks toward its new
 * width instead of snapping there. Self-contained: always starts by
 * blanking the bar's own rect, so it never depends on what a
 * previous frame left behind, same principle every other redraw in
 * this file already follows. `frame` (1..ANIM_FRAMES, combined with
 * the cross-call anim_tick) only drives the shimmer sweep's phase. */
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

  /* A soft ~8px band that sweeps across whatever's currently filled,
   * `frame`- and anim_tick-driven so it keeps moving both within one
   * transition and across separate splash_progress() calls, instead
   * of resetting to the same spot every time this runs. Triangular
   * alpha across its own width so it fades in and out at both edges
   * rather than reading as a hard-edged stripe. */
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

  /* A soft leading edge instead of a hard vertical cut. */
  for (uint32_t i = 0; i < 3 && fill_w + i < bar_w; i++) {
    uint8_t edge_alpha = (uint8_t)(170 - i * 60);
    for (uint32_t y = 0; y < bar_h; y++) {
      fb_blend_pixel(bar_x + fill_w + i, bar_y + y, COLOR_BAR_FILL, edge_alpha);
    }
  }
}

/* Renders `reveal_chars` characters of `label`, left to right, at a
 * FIXED x position computed from the label's FULL width -- not the
 * partial string's width -- so each newly revealed character appears
 * in place instead of the whole line shifting as it "types". Same
 * self-contained-redraw shape as draw_bar_frame(): always blanks its
 * own band first. */
static void draw_status_partial(const char *label, uint32_t reveal_chars) {
  fb_fill_rect(0, status_y, fb_width(), GLYPH_SRC * status_scale + 4,
               status_band_color);

  uint32_t cell = GLYPH_SRC * status_scale + status_scale;
  uint32_t full_w = text_width(label, status_scale, status_scale);
  uint64_t fbw = fb_width();
  uint32_t x = (fbw > full_w) ? (uint32_t)((fbw - full_w) / 2) : 0;

  uint32_t shown = 0;
  for (const char *p = label; *p != '\0' && shown < reveal_chars;
       p++, shown++) {
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

  /* --- layout: figure out every Y position before drawing anything,
   * so the background gradient (drawn first, underneath everything)
   * knows where the "content band" actually is. --- */
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

  /* Gradient panel: a known-good direct fill per row (the screen was
   * JUST cleared to NX_COLOR_BG above, so the target color is known
   * without reading anything back) -- cheaper than blending every
   * pixel for something this size, see mix_rgb()'s own comment. */
  for (uint32_t y = panel_top; y < panel_bottom; y++) {
    uint32_t row_color = mix_rgb(NX_COLOR_BG, COLOR_PANEL, panel_alpha_at(y));
    fb_fill_rect(0, y, w, 1, row_color);
  }

  draw_dust();

  uint32_t wordmark_w = text_width(WORDMARK, logo_scale, logo_scale / 2);
  draw_glow((uint32_t)(w / 2), logo_y + logo_h / 2, wordmark_w, logo_h);

  /* Letter-by-letter reveal instead of an instant blit -- see
   * draw_text_scaled_animated()'s own comment for why this reads as
   * the wordmark emerging out of the glow already drawn behind it. */
  draw_text_scaled_animated(logo_y, WORDMARK, logo_scale, logo_scale / 2,
                            NX_COLOR_ACCENT, true, ANIM_WORDMARK_LETTER_MS);
  draw_text_scaled_animated(tagline_y, TAGLINE, tagline_scale, tagline_scale,
                            NX_COLOR_DIM, true, ANIM_TAGLINE_LETTER_MS);

  fb_fill_rect(bar_x - 2, bar_y - 2, bar_w + 4, bar_h + 4, COLOR_BAR_BORDER);
  fb_fill_rect(bar_x, bar_y, bar_w, bar_h, COLOR_BAR_EMPTY);

  /* Soften the frame's 4 corners a touch, blending toward the panel
   * gradient's own color at that exact row rather than a flat
   * constant, so the fade matches whatever's actually behind it. */
  uint32_t fx0 = bar_x - 2, fy0 = bar_y - 2;
  uint32_t fx1 = bar_x + bar_w + 1, fy1 = bar_y + bar_h + 1;
  uint32_t top_bg = mix_rgb(NX_COLOR_BG, COLOR_PANEL, panel_alpha_at(fy0));
  uint32_t bot_bg = mix_rgb(NX_COLOR_BG, COLOR_PANEL, panel_alpha_at(fy1));
  fb_blend_pixel(fx0, fy0, top_bg, 200);
  fb_blend_pixel(fx1, fy0, top_bg, 200);
  fb_blend_pixel(fx0, fy1, bot_bg, 200);
  fb_blend_pixel(fx1, fy1, bot_bg, 200);

  status_band_color =
      mix_rgb(NX_COLOR_BG, COLOR_PANEL, panel_alpha_at(status_y));

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

  /* Plays the transition out over ANIM_FRAMES frames instead of
   * snapping straight to the new state -- the bar eases toward
   * `target_w` (see the frac_num/frac_den math below, an integer
   * ease-out quadratic: fast at first, settling in toward the end,
   * so it reads as the bar "catching up" rather than crawling at a
   * constant rate) while the status label types itself in alongside
   * it, both driven by the same frame loop so they stay in step. */
  for (uint32_t frame = 1; frame <= ANIM_FRAMES; frame++) {
    uint32_t rem = ANIM_FRAMES - frame;
    uint32_t frac_num = ANIM_FRAMES * ANIM_FRAMES - rem * rem;
    uint32_t frac_den = ANIM_FRAMES * ANIM_FRAMES;

    uint32_t interp_w;
    if (target_w >= start_w) {
      interp_w =
          start_w +
          (uint32_t)(((uint64_t)(target_w - start_w) * frac_num) / frac_den);
    } else {
      interp_w =
          start_w -
          (uint32_t)(((uint64_t)(start_w - target_w) * frac_num) / frac_den);
    }

    draw_bar_frame(interp_w, frame);

    uint32_t reveal =
        (uint32_t)(((uint64_t)label_len * frame + ANIM_FRAMES - 1) /
                   ANIM_FRAMES);
    draw_status_partial(label, reveal);

    pit_wait_ms(ANIM_FRAME_MS);
  }

  anim_fill_w = target_w; /* exact final state -- no rounding drift
                              carried into the next call */
  anim_tick++;
}

void splash_finish(void) {
  if (!splash_active) {
    return;
  }
  splash_active = false;

  /* Sweeps the screen clear left to right in ANIM_WIPE_STEPS vertical
   * strips, with a thin bright leading edge riding just ahead of the
   * cleared area -- like a curtain being pulled back, rather than the
   * whole screen cutting to blank in one frame. Deliberately a wipe
   * (direct fb_fill_rect()s) rather than a full-screen
   * fb_blend_pixel() fade: unlike every other animation in this file,
   * a fade would have to touch every pixel to erase the dust/glow
   * underneath it, and fb_blend_pixel() does a genuine read-modify-
   * write per pixel (see fb.c) -- multiplied by several frames across
   * a whole framebuffer, that cost stops being negligible. A wipe
   * only ever draws each pixel once, total, across the whole
   * transition, so it stays cheap regardless of resolution. */
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
