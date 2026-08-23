#include "acpi/acpi.h"
#include "apic/ioapic.h"
#include "apic/lapic.h"
#include "boot/requests.h"
#include "cpu/cpu.h"
#include "cpu/gdt.h"
#include "cpu/idt.h"
#include "cpu/io.h"
#include "cpu/isr.h"
#include "cpu/syscall.h"
#include "debug/log.h"
#include "debug/serial.h"
#include "drivers/keyboard.h"
#include "drivers/nvme.h"
#include "drivers/virtio_blk.h"
#include "fs/graph.h"
#include "fs/graphfs_vfs.h"
#include "fs/initrd.h"
#include "fs/tmpfs.h"
#include "fs/vfs.h"
#include "klib/klib.h"
#include "mm/heap.h"
#include "mm/pmm.h"
#include "mm/vmm.h"
#include "panic.h"
#include "proc/process.h"
#include "sched/sched.h"
#include "shell/shell.h"
#include "smp/smp.h"
#include "time/timer.h"
#include "video/console.h"
#include "video/fb.h"
#include "video/splash.h"
#define GRAPH_AUTOSAVE_INTERVAL_MS (30 * 1000)

static void heartbeat_task(void *arg) {
  const char *name = (const char *)arg;
  for (;;) {
    kprintf("[%s] cpu%u alive, uptime %lu ms, %u task(s) live\n", name,
            this_cpu()->cpu_index, timer_uptime_ms(), sched_task_count());
    sched_sleep_ms(5000);
  }
}

static void graph_autosave_task(void *arg) {
  (void)arg;
  for (;;) {
    sched_sleep_ms(GRAPH_AUTOSAVE_INTERVAL_MS);
    if (graph_is_dirty()) {
      graph_save_to_disk(); /* logs its own outcome; a failure leaves
                                dirty set, so the next cycle retries */
    }
  }
}
static void blockdev_init_task(void *arg) {
  (void)arg;
  bool have_disk = nvme_init();
  if (!have_disk) {
    have_disk = virtio_blk_init();
  }
  if (have_disk) {
    graph_load_from_disk();
  }
  task_exit();
}

static void selftest_task(void *arg) {
  (void)arg;
  kprintf("[selftest] spawning /bin/hello ...\n");
  struct task *t = process_spawn("/bin/hello", "hello", 0);
  if (t == NULL) {
    kprintf("[selftest] FAIL: could not spawn /bin/hello\n");
    task_exit();
  }
  int code = process_wait(t);
  kprintf("[selftest] /bin/hello exited with code %d\n", code);
  kprintf("[selftest] %s\n", code == 0 ? "PASS" : "FAIL");
  task_exit();
}

static void print_banner(void) {
  kprintf("\n");
  kprintf(" _   _\n");
  kprintf("| \\ | | _____  ___   _ ___\n");
  kprintf("|  \\| |/ _ \\ \\/ / | | / __|\n");
  kprintf("| |\\  |  __/>  <| |_| \\__ \\\n");
  kprintf("|_| \\_|\\___/_/\\_\\\\__,_|___/\n");
  kprintf("\n");
  kprintf("Nexus OS -- x86-64 / SMP / x2APIC -- booting via Limine\n");
  kprintf("bootloader: %s %s\n", g_boot.bootloader_name,
          g_boot.bootloader_version);
  kprintf("cmdline:    \"%s\"\n\n", g_boot.cmdline);
}

NORETURN void kmain(void) {
  serial_init();
  boot_requests_init();

  /* No dependencies at all -- safe (and necessary, see the ordering
   * note on vmm_init() below) to do this before anything else. */
  isr_init();

  /* Must happen before any page table with an NX-marked entry becomes
   * active, which vmm_init() below does. */
  cpu_enable_nx();

  fb_init();
  console_init();

  /* splash_show() suspends the console (see console_set_suspended())
   * and takes the framebuffer over directly -- everything below still
   * logs to serial exactly as before, it just doesn't show up on
   * screen until splash_finish() hands the framebuffer back. A no-op
   * on a serial-only boot (no framebuffer), so print_banner()'s
   * ordinary scrolling text is exactly what shows up in that case. */
  splash_show();
  print_banner();

  pmm_init();
  splash_progress(10, "physical memory");

  vmm_init();
  splash_progress(20, "page tables");

  struct cpu_local *bsp = cpu_local_create(0, 0, 0, true);
  smp_register_bsp(bsp);
  cpu_setup_current(bsp); /* installs our GDT/TSS, sets GS_BASE */

  idt_init();
  syscall_init();
  panic_init();
  splash_progress(30, "cpu bring-up");

  acpi_init();
  splash_progress(40, "ACPI");

  lapic_init();
  bsp->lapic_id = lapic_id();
  bsp->online = true;

  ioapic_init();
  splash_progress(50, "interrupt controllers");

  heap_init();
  vfs_init();
  vfs_set_root(tmpfs_create_root());
  graph_init();
  const struct limine_file *initrd = boot_find_module("initrd.tar");
  if (initrd != NULL) {
    initrd_unpack(initrd->address, initrd->size);
  } else {
    kprintf("[boot] no initrd module found -- '/' will be empty\n");
  }

  vfs_set_root_fallback(graphfs_vfs_root());
  splash_progress(65, "filesystem");

  timer_calibrate();

  sched_init();
  splash_progress(80, "scheduler");

  smp_init(); /* releases APs; fire-and-forget, see smp.c */

  timer_start_periodic_for_this_cpu(); /* the BSP's own tick */
  splash_progress(90, "SMP");

  keyboard_init(bsp->lapic_id);
  splash_progress(100, "ready");
  splash_finish();

  kprintf("[boot] core init complete, starting the scheduler\n\n");

  task_create("shell", shell_task, NULL);
  task_create("blockdev", blockdev_init_task, NULL);
  task_create("gautosave", graph_autosave_task, NULL);
  if (strstr(g_boot.cmdline, "selftest") != NULL) {
    task_create("selftest", selftest_task, NULL);
  }
  // task_create("heartbeat-a", heartbeat_task, "heartbeat-a");
  // task_create("heartbeat-b", heartbeat_task, "heartbeat-b");

  sched_enter_idle(); /* this CPU becomes idle/bsp; never returns */
}
