#ifndef NEXUS_SPLASH_H
#define NEXUS_SPLASH_H

#include "klib/klib.h"

// Shows the boot splash and suspends framebuffer console output. No-op without a framebuffer.
void splash_show(void);

// Updates the splash progress bar and status text. `percent` is clamped to [0, 100].
void splash_progress(uint32_t percent, const char *label);

// Closes the splash and restores the console.
void splash_finish(void);

#endif /* NEXUS_SPLASH_H */
