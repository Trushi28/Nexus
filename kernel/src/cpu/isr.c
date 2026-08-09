#include "cpu/isr.h"
#include "apic/lapic.h"
#include "cpu/io.h"
#include "cpu/vectors.h"
#include "debug/log.h"
#include "panic.h"

static interrupt_handler_t handlers[256];

static const char *exception_name(uint64_t vector) {
  static const char *names[32] = {
      "Divide Error",
      "Debug",
      "NMI",
      "Breakpoint",
      "Overflow",
      "BOUND Range Exceeded",
      "Invalid Opcode",
      "Device Not Available",
      "Double Fault",
      "Coprocessor Segment Overrun",
      "Invalid TSS",
      "Segment Not Present",
      "Stack-Segment Fault",
      "General Protection Fault",
      "Page Fault",
      "Reserved",
      "x87 Floating-Point Exception",
      "Alignment Check",
      "Machine Check",
      "SIMD Floating-Point Exception",
      "Virtualization Exception",
      "Control Protection Exception",
      "Reserved",
      "Reserved",
      "Reserved",
      "Reserved",
      "Reserved",
      "Reserved",
      "Hypervisor Injection Exception",
      "VMM Communication Exception",
      "Security Exception",
      "Reserved",
  };
  if (vector < 32) {
    return names[vector];
  }
  return "Unknown";
}

void register_interrupt_handler(uint8_t vector, interrupt_handler_t handler) {
  handlers[vector] = handler;
}

void isr_init(void) {
  for (int i = 0; i < 256; i++) {
    handlers[i] = NULL;
  }
}

static void dump_and_panic(struct interrupt_frame *f) {
  uint64_t cr2 = (f->vector == 14) ? read_cr2() : 0;

  kprintf("\n*** UNHANDLED EXCEPTION %u: %s (error=0x%x) ***\n",
          (unsigned)f->vector, exception_name(f->vector),
          (unsigned)f->error_code);
  if (f->vector == 14) {
    kprintf("  faulting address (CR2) = 0x%p\n", (void *)cr2);
    kprintf("  %s%s%s, %s-mode, %s\n",
            (f->error_code & 1) ? "present" : "not-present",
            (f->error_code & 2) ? ", write" : ", read",
            (f->error_code & 4) ? ", user" : ", supervisor",
            (f->error_code & 4) ? "user" : "kernel",
            (f->error_code & 16) ? "instruction fetch" : "data access");
  }
  kprintf("  rip=0x%p cs=0x%x rflags=0x%p\n", (void *)f->rip, (unsigned)f->cs,
          (void *)f->rflags);
  kprintf("  rsp=0x%p ss=0x%x\n", (void *)f->rsp, (unsigned)f->ss);
  kprintf("  rax=0x%p rbx=0x%p rcx=0x%p rdx=0x%p\n", (void *)f->rax,
          (void *)f->rbx, (void *)f->rcx, (void *)f->rdx);
  kprintf("  rsi=0x%p rdi=0x%p rbp=0x%p\n", (void *)f->rsi, (void *)f->rdi,
          (void *)f->rbp);
  kprintf("  r8=0x%p  r9=0x%p  r10=0x%p r11=0x%p\n", (void *)f->r8,
          (void *)f->r9, (void *)f->r10, (void *)f->r11);
  kprintf("  r12=0x%p r13=0x%p r14=0x%p r15=0x%p\n", (void *)f->r12,
          (void *)f->r13, (void *)f->r14, (void *)f->r15);

  panic("unhandled CPU exception");
}

void isr_common_handler(struct interrupt_frame *frame) {
  uint64_t v = frame->vector;
  if (v >= VEC_IRQ_BASE && v != VEC_SYSCALL && v != VEC_SPURIOUS) {
    lapic_send_eoi();
  }

  if (handlers[v] != NULL) {
    handlers[v](frame);
    return;
  }

  if (v < 32) {
    dump_and_panic(frame);
    return;
  }

  if (v == VEC_SPURIOUS) {
    return; /* spurious vector: no EOI required by the APIC spec */
  }

  /* Unregistered IRQ/IPI: already ack'd above so the APIC doesn't
   * wedge, but make some noise since it means we forgot to wire
   * something up. */
  kprintf("[isr] unhandled vector %u\n", (unsigned)v);
}
