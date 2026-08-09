#ifndef NEXUS_KEYBOARD_H
#define NEXUS_KEYBOARD_H

#include "klib/klib.h"

/* Routes IRQ1 through the I/O APIC to `dest_apic_id` (normally the BSP)
 * and installs the scancode-decoding ISR. Call after ioapic_init(). */
void keyboard_init(uint32_t dest_apic_id);

/* Blocks the calling task until a key is available, then returns its
 * decoded ASCII value. Backspace is '\b', Enter is '\n'. Unrecognised/
 * non-printable keys (function keys, arrows, ...) are simply not
 * produced -- there's no terminal escape-sequence handling in v1. */
char keyboard_getc(void);

/* Non-blocking: true if at least one decoded keystroke is waiting to
 * be read via keyboard_getc(). For UIs (like the `matrix` shell
 * command) that want to poll for "any key to stop" without blocking. */
bool keyboard_haskey(void);

/* Discards any buffered-but-unread keystrokes. Handy right before
 * "press any key to stop"-style polling loops, so a key mashed a
 * moment earlier doesn't end the loop instantly, and right after, so
 * that same keypress doesn't leak into whatever reads input next. */
void keyboard_flush(void);

#endif /* NEXUS_KEYBOARD_H */
