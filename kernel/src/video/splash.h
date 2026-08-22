#ifndef NEXUS_SPLASH_H
#define NEXUS_SPLASH_H

#include "klib/klib.h"

/* Draws the full-screen boot splash -- a scaled "NEXUS" wordmark, a
 * tagline, and an empty progress bar -- and suspends the text console
 * (see console_set_suspended() in video/console.h) so kprintf()'s
 * ordinary boot-log spam keeps going to serial but doesn't scribble
 * text over the splash art. A no-op if there's no framebuffer
 * (fb_available() false) -- kmain() falls straight through to the
 * ordinary scrolling console in that case, exactly like before this
 * existed. Call once, right after fb_init()/console_init(). */
void splash_show(void);

/* Updates the progress bar's fill and the status line underneath it,
 * in place -- never redraws the logo/tagline. `percent` is clamped to
 * [0, 100]. A no-op if splash_show() was never called (or there's no
 * framebuffer). Call this at each boot milestone in kmain(). */
void splash_progress(uint32_t percent, const char *label);

/* Clears the splash, un-suspends the console, and clears the console
 * to a blank screen ready for the ordinary boot log / shell. Call
 * once, right before handing off to the scheduler. A no-op if
 * splash_show() was never called. */
void splash_finish(void);

#endif /* NEXUS_SPLASH_H */
