#include "apic/ioapic.h"
#include "cpu/io.h"
#include "cpu/isr.h"
#include "cpu/vectors.h"
#include "drivers/keyboard.h"
#include "sched/sched.h"

#define KBD_DATA_PORT 0x60
#define KBD_CMD_PORT 0x64

/* Scan Code Set 1, unshifted and shifted, US QWERTY. 0 = "no ASCII
 * mapping for this scancode" (function keys, arrows, etc. -- not
 * handled in v1). */
static const char unshifted[128] = {
    [0x02] = '1', [0x03] = '2',  [0x04] = '3',  [0x05] = '4',  [0x06] = '5',
    [0x07] = '6', [0x08] = '7',  [0x09] = '8',  [0x0A] = '9',  [0x0B] = '0',
    [0x0C] = '-', [0x0D] = '=',  [0x0E] = '\b', [0x0F] = '\t', [0x10] = 'q',
    [0x11] = 'w', [0x12] = 'e',  [0x13] = 'r',  [0x14] = 't',  [0x15] = 'y',
    [0x16] = 'u', [0x17] = 'i',  [0x18] = 'o',  [0x19] = 'p',  [0x1A] = '[',
    [0x1B] = ']', [0x1C] = '\n', [0x1E] = 'a',  [0x1F] = 's',  [0x20] = 'd',
    [0x21] = 'f', [0x22] = 'g',  [0x23] = 'h',  [0x24] = 'j',  [0x25] = 'k',
    [0x26] = 'l', [0x27] = ';',  [0x28] = '\'', [0x29] = '`',  [0x2B] = '\\',
    [0x2C] = 'z', [0x2D] = 'x',  [0x2E] = 'c',  [0x2F] = 'v',  [0x30] = 'b',
    [0x31] = 'n', [0x32] = 'm',  [0x33] = ',',  [0x34] = '.',  [0x35] = '/',
    [0x39] = ' ',
};

static const char shifted[128] = {
    [0x02] = '!', [0x03] = '@',  [0x04] = '#',  [0x05] = '$',  [0x06] = '%',
    [0x07] = '^', [0x08] = '&',  [0x09] = '*',  [0x0A] = '(',  [0x0B] = ')',
    [0x0C] = '_', [0x0D] = '+',  [0x0E] = '\b', [0x0F] = '\t', [0x10] = 'Q',
    [0x11] = 'W', [0x12] = 'E',  [0x13] = 'R',  [0x14] = 'T',  [0x15] = 'Y',
    [0x16] = 'U', [0x17] = 'I',  [0x18] = 'O',  [0x19] = 'P',  [0x1A] = '{',
    [0x1B] = '}', [0x1C] = '\n', [0x1E] = 'A',  [0x1F] = 'S',  [0x20] = 'D',
    [0x21] = 'F', [0x22] = 'G',  [0x23] = 'H',  [0x24] = 'J',  [0x25] = 'K',
    [0x26] = 'L', [0x27] = ':',  [0x28] = '"',  [0x29] = '~',  [0x2B] = '|',
    [0x2C] = 'Z', [0x2D] = 'X',  [0x2E] = 'C',  [0x2F] = 'V',  [0x30] = 'B',
    [0x31] = 'N', [0x32] = 'M',  [0x33] = '<',  [0x34] = '>',  [0x35] = '?',
    [0x39] = ' ',
};

#define RING_SIZE 256
static char ring[RING_SIZE];
static volatile uint32_t ring_head = 0,
                         ring_tail = 0; /* head=write, tail=read */

static struct wait_queue kbd_wq;
static bool shift_held = false;

static void ring_push(char c) {
  uint32_t next = (ring_head + 1) % RING_SIZE;
  if (next == ring_tail) {
    return; /* full: drop the oldest-pending keystroke's slot silently */
  }
  ring[ring_head] = c;
  ring_head = next;
}

static void keyboard_isr(struct interrupt_frame *frame) {
  (void)frame;
  uint8_t sc = inb(KBD_DATA_PORT);
  bool release = (sc & 0x80) != 0;
  uint8_t code = sc & 0x7F;

  if (code == 0x2A || code == 0x36) { /* left/right shift */
    shift_held = !release;
    return;
  }

  if (release) {
    return;
  }

  char c = shift_held ? shifted[code] : unshifted[code];
  if (c != 0) {
    ring_push(c);
    wait_queue_wake(&kbd_wq);
  }
}

void keyboard_init(uint32_t dest_apic_id) {
  /* 1. Flush the 8042 PS/2 data buffer. UEFI leaves garbage bytes
   * hanging here that will deadlock the controller if ignored. */
  while (inb(KBD_CMD_PORT) & 0x01) {
    inb(KBD_DATA_PORT);
  }

  /* 2. Enable the primary PS/2 port */
  outb(KBD_CMD_PORT, 0xAE);

  /* 3. Read the Configuration Byte */
  outb(KBD_CMD_PORT, 0x20);
  while (!(inb(KBD_CMD_PORT) & 0x01)) {
    asm volatile("pause");
  }
  uint8_t config = inb(KBD_DATA_PORT);

  /* 4. Enable port 1 interrupts (bit 0) and hardware translation (bit 6) */
  config |= (1 << 0) | (1 << 6);

  /* 5. Write the Configuration Byte back */
  outb(KBD_CMD_PORT, 0x60);
  outb(KBD_DATA_PORT, config);

  ioapic_set_irq(1, VEC_KEYBOARD, dest_apic_id);
  register_interrupt_handler(VEC_KEYBOARD, keyboard_isr);
}

char keyboard_getc(void) {
  for (;;) {
    if (ring_tail != ring_head) {
      char c = ring[ring_tail];
      ring_tail = (ring_tail + 1) % RING_SIZE;
      return c;
    }
    wait_queue_block(&kbd_wq);
  }
}

bool keyboard_haskey(void) {
  return ring_tail != ring_head;
}

void keyboard_flush(void) {
  ring_tail = ring_head;
}
