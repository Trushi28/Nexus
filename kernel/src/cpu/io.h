#ifndef NEXUS_IO_H
#define NEXUS_IO_H

#include "klib/klib.h"

static inline void outb(uint16_t port, uint8_t val) {
    asm volatile ("outb %0, %1" : : "a"(val), "Nd"(port));
}

static inline uint8_t inb(uint16_t port) {
    uint8_t ret;
    asm volatile ("inb %1, %0" : "=a"(ret) : "Nd"(port));
    return ret;
}

static inline void outw(uint16_t port, uint16_t val) {
    asm volatile ("outw %0, %1" : : "a"(val), "Nd"(port));
}

static inline uint16_t inw(uint16_t port) {
    uint16_t ret;
    asm volatile ("inw %1, %0" : "=a"(ret) : "Nd"(port));
    return ret;
}

static inline void outl(uint16_t port, uint32_t val) {
    asm volatile ("outl %0, %1" : : "a"(val), "Nd"(port));
}

static inline uint32_t inl(uint16_t port) {
    uint32_t ret;
    asm volatile ("inl %1, %0" : "=a"(ret) : "Nd"(port));
    return ret;
}

/* Standard trick to give the bus a moment on ancient hardware that needs
 * it. Port 0x80 is a POST-code scratch port that is safe to write to for
 * timing purposes on essentially every PC-compatible ever made. */
static inline void io_wait(void) {
    outb(0x80, 0);
}

static inline void io_delay_cycles(uint32_t n) {
    for (volatile uint32_t i = 0; i < n; i++) {
        asm volatile ("pause");
    }
}

static inline uint64_t rdmsr(uint32_t msr) {
    uint32_t lo, hi;
    asm volatile ("rdmsr" : "=a"(lo), "=d"(hi) : "c"(msr));
    return ((uint64_t)hi << 32) | lo;
}

static inline void wrmsr(uint32_t msr, uint64_t val) {
    uint32_t lo = (uint32_t)(val & 0xFFFFFFFF);
    uint32_t hi = (uint32_t)(val >> 32);
    asm volatile ("wrmsr" : : "a"(lo), "d"(hi), "c"(msr));
}

static inline uint64_t rdtsc(void) {
    uint32_t lo, hi;
    asm volatile ("rdtsc" : "=a"(lo), "=d"(hi));
    return ((uint64_t)hi << 32) | lo;
}

static inline void cpuid(uint32_t leaf, uint32_t subleaf, uint32_t *eax, uint32_t *ebx, uint32_t *ecx, uint32_t *edx) {
    asm volatile ("cpuid"
                  : "=a"(*eax), "=b"(*ebx), "=c"(*ecx), "=d"(*edx)
                  : "a"(leaf), "c"(subleaf));
}

static inline uint64_t read_cr0(void) { uint64_t v; asm volatile ("mov %%cr0, %0" : "=r"(v)); return v; }
static inline uint64_t read_cr2(void) { uint64_t v; asm volatile ("mov %%cr2, %0" : "=r"(v)); return v; }
static inline uint64_t read_cr3(void) { uint64_t v; asm volatile ("mov %%cr3, %0" : "=r"(v)); return v; }
static inline uint64_t read_cr4(void) { uint64_t v; asm volatile ("mov %%cr4, %0" : "=r"(v)); return v; }

static inline void write_cr3(uint64_t v) {
    asm volatile ("mov %0, %%cr3" : : "r"(v) : "memory");
}

static inline void write_cr4(uint64_t v) {
    asm volatile ("mov %0, %%cr4" : : "r"(v) : "memory");
}

static inline void invlpg(uint64_t vaddr) {
    asm volatile ("invlpg (%0)" : : "r"(vaddr) : "memory");
}

static inline void cli(void) { asm volatile ("cli"); }
static inline void sti(void) { asm volatile ("sti"); }
static inline void hlt(void) { asm volatile ("hlt"); }

static inline uint64_t read_rflags(void) {
    uint64_t f;
    asm volatile ("pushfq; pop %0" : "=r"(f));
    return f;
}

static inline NORETURN void hang(void) {
    cli();
    for (;;) {
        hlt();
    }
}

#endif /* NEXUS_IO_H */
