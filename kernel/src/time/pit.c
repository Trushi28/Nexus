#include "time/pit.h"
#include "cpu/io.h"

#define PIT_HZ 1193182u
#define PIT_CH0_DATA 0x40
#define PIT_COMMAND  0x43

static uint16_t pit_latch_read(void) {
    outb(PIT_COMMAND, 0x00); // latch channel 0's current count
    uint8_t lo = inb(PIT_CH0_DATA);
    uint8_t hi = inb(PIT_CH0_DATA);
    return (uint16_t)(lo | (hi << 8));
}

void pit_wait_ms(uint32_t ms) {
    uint32_t reload = (uint32_t)(((uint64_t)PIT_HZ * ms) / 1000);
    if (reload == 0) {
        reload = 1;
    }
    if (reload > 0xFFFF) {
        reload = 0xFFFF; // caller asked for more than one channel period covers
    }

    // Channel 0, lobyte/hibyte access, mode 2 (rate generator, auto reload), binary mode.
    outb(PIT_COMMAND, 0x34);
    outb(PIT_CH0_DATA, (uint8_t)(reload & 0xFF));
    outb(PIT_CH0_DATA, (uint8_t)((reload >> 8) & 0xFF));

    /* Mode 2 counts down to 1, pulses, then reloads and starts again.
     * Poll until we see the count jump back up -- that's one full period
     * having elapsed. */
    uint16_t last = pit_latch_read();
    for (;;) {
        uint16_t cur = pit_latch_read();
        if (cur > last) {
            break;
        }
        last = cur;
    }
}
