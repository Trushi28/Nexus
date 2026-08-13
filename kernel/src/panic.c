#include "panic.h"
#include "apic/lapic.h"
#include "cpu/io.h"
#include "cpu/isr.h"
#include "cpu/vectors.h"
#include "debug/log.h"

static void panic_halt_handler(struct interrupt_frame *frame) {
  (void)frame;
  hang();
}

void panic_init(void) {
  register_interrupt_handler(VEC_PANIC_HALT_IPI, panic_halt_handler);
}

NORETURN void panic(const char *fmt, ...) {
  cli();

  char msg[256];
  va_list ap;
  va_start(ap, fmt);
  kvsnprintf(msg, sizeof(msg), fmt, ap);
  va_end(ap);

  kprintf("\n"
          "================================================================\n"
          "  NEXUS PANIC: %s\n"
          "================================================================\n",
          msg);

  /* Best-effort: ask every other core to stop too. If the APIC isn't up
   * yet (very early panic) this is skipped gracefully. */
  if (lapic_is_ready()) {
    lapic_send_ipi_all_excluding_self(VEC_PANIC_HALT_IPI);
  }
  hang();
}
