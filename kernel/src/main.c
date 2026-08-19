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
static void nvme_init_task(void *arg) {
  (void)arg;
  if (nvme_init()) {
    graph_load_from_disk(); /* best-effort -- logs its own outcome,
                                including the ordinary "nothing saved
                                yet" case on a fresh disk */
  }
  task_exit();
}
static void selftest_task(void *arg) {
  (void)arg;
  kprintf("[selftest] spawning /bin/hello ...\n");
  struct task *t = process_spawn("/bin/hello", "hello");
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
  print_banner();

  pmm_init();

  /* vmm_init() registers the TLB-shootdown IPI handler as part of
   * bringing up our own page tables -- isr_init() above must have run
   * first, or this registration would be wiped out. */
  vmm_init();

  struct cpu_local *bsp = cpu_local_create(0, 0, 0, true);
  smp_register_bsp(bsp);
  cpu_setup_current(bsp); /* installs our GDT/TSS, sets GS_BASE */

  /* idt_init() builds gates pointing at GDT_KERNEL_CODE (0x08), which
   * is only meaningful once our own GDT (just installed above) is the
   * one that's loaded. */
  idt_init();
  syscall_init();
  panic_init();

  acpi_init();

  lapic_init();
  bsp->lapic_id = lapic_id();
  bsp->online = true;

  ioapic_init();

  heap_init();

  /* Mount tmpfs at "/" and unpack the initrd module into it -- both
   * need the heap (kmalloc-backed vnodes/file buffers), nothing else.
   * A missing module just leaves "/" empty; not fatal, since a
   * from-scratch OS is still perfectly usable without a filesystem. */
  vfs_set_root(tmpfs_create_root());
  graph_init();
  const struct limine_file *initrd = boot_find_module("initrd.tar");
  if (initrd != NULL) {
    initrd_unpack(initrd->address, initrd->size);
  } else {
    kprintf("[boot] no initrd module found -- '/' will be empty\n");
  }

  /* From here on, any top-level name that isn't a real tmpfs entry
   * falls through to the graph filesystem: `sstring photos <node>`
   * makes "photos" work as an ordinary top-level path immediately
   * (ls, cat, run, ring-3's open() with O_CREAT/O_TRUNC -- all of
   * it), and a brand-new top-level name created through open(O_CREAT)
   * becomes a new graph node auto-anchored under that same name -- no
   * separate mount point, no /graph prefix. Deliberately registered
   * only NOW, after the initrd is already unpacked: vfs_set_root_fallback()
   * makes every *subsequent* top-level create prefer the graph over
   * tmpfs (see fs/vfs.c's walk()), and initrd_unpack()'s own
   * vfs_lookup_or_create() calls for "bin" etc. are top-level creates
   * too -- registering the fallback any earlier would silently pull
   * the whole initrd into the graph instead of tmpfs. See
   * fs/graphfs_vfs.c for the adapter itself. */
  vfs_set_root_fallback(graphfs_vfs_root());

  timer_calibrate();

  smp_init(); /* releases APs; fire-and-forget, see smp.c */

  timer_start_periodic_for_this_cpu(); /* the BSP's own tick */

  sched_init();

  keyboard_init(bsp->lapic_id);

  kprintf("[boot] core init complete, starting the scheduler\n\n");

  task_create("shell", shell_task, NULL);
  task_create("nvme", nvme_init_task, NULL);
  task_create("gautosave", graph_autosave_task, NULL);
  if (strstr(g_boot.cmdline, "selftest") != NULL) {
    task_create("selftest", selftest_task, NULL);
  }
  // task_create("heartbeat-a", heartbeat_task, "heartbeat-a");
  // task_create("heartbeat-b", heartbeat_task, "heartbeat-b");

  sched_enter_idle(); /* this CPU becomes idle/bsp; never returns */
}
