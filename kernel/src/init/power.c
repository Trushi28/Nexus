#include "init/power.h"
#include "acpi/acpi.h"
#include "cpu/io.h"
#include "debug/log.h"
#include "fs/graph.h"
#include "init/loom.h"
#include "time/timer.h"

NORETURN void power_reboot(void) {
  loom_shutdown_all();
  if (graph_is_dirty()) {
    kprintf("graph has unsaved changes -- saving before reboot...\n");
    graph_save_to_disk(); /* logs its own outcome; a failed save still
                              falls through to reboot -- refusing to
                              reboot over a save failure would be
                              worse */
  }
  kprintf("rebooting...\n");
  timer_busy_wait_ms(50);
  uint8_t status;
  do {
    status = inb(0x64);
  } while (status & 0x02);
  outb(0x64, 0xFE);
  hang();
}

NORETURN void power_shutdown(void) {
  loom_shutdown_all();
  if (graph_is_dirty()) {
    kprintf("graph has unsaved changes -- saving before shutdown...\n");
    graph_save_to_disk();
  }
  kprintf("shutting down...\n");
  timer_busy_wait_ms(50);
  acpi_shutdown(); /* never returns -- real ACPI / QEMU-hack / halt
                       fallback chain lives in acpi.c */
}
