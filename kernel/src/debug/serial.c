#include "debug/serial.h"
#include "cpu/io.h"

#define COM1 0x3F8

void serial_init(void) {
    outb(COM1 + 1, 0x00); /* disable interrupts */
    outb(COM1 + 3, 0x80); /* enable DLAB */
    outb(COM1 + 0, 0x01); /* divisor low byte -> 115200 baud */
    outb(COM1 + 1, 0x00); /* divisor high byte */
    outb(COM1 + 3, 0x03); /* 8 bits, no parity, one stop bit; DLAB off */
    outb(COM1 + 2, 0xC7); /* enable + clear 14-byte-threshold FIFO */
    outb(COM1 + 4, 0x0B); /* IRQs disabled at the UART, RTS/DSR set */
}

static bool transmit_empty(void) {
    return (inb(COM1 + 5) & 0x20) != 0;
}

void serial_putc(char c) {
    if (c == '\n') {
        serial_putc('\r');
    }
    while (!transmit_empty()) {
        asm volatile ("pause");
    }
    outb(COM1, (uint8_t)c);
}

void serial_puts(const char *s) {
    while (*s) {
        serial_putc(*s++);
    }
}
