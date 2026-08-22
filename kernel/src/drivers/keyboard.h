#ifndef NEXUS_KEYBOARD_H
#define NEXUS_KEYBOARD_H
#define KEY_UP 0x01
#define KEY_DOWN 0x02

#include "klib/klib.h"

void keyboard_init(uint32_t dest_apic_id);

char keyboard_getc(void);

bool keyboard_haskey(void);

void keyboard_flush(void);

#endif /* NEXUS_KEYBOARD_H */
