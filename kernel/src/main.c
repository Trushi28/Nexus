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
#include "fs/vfs.h"
#include "init/loom.h"
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

static void blockdev_init_task(void *arg) {
  (void)arg;

  /* NVMe/virtio-blk bring-up blocks on a real completion interrupt
   * (submit_and_wait() -> task_block() -> schedule()), so it can only
   * run once the scheduler exists -- this task is why boot doesn't
   * try to do any of this synchronously in kmain(). That in turn is
   * why GraphFS -- now the actual "/" (see vfs_set_root() in kmain())
   * -- can only be guaranteed populated from HERE onward, not any
   * earlier: this task owns disk bring-up, restoring last session's
   * graph, seeding/refreshing '/bin' from this build's initrd, and
   * only THEN handing off to an interactive shell and (if requested)
   * the self-test -- every one of which needs '/bin' to actually
   * exist first. */
  bool have_disk = nvme_init();
  if (!have_disk) {
    have_disk = virtio_blk_init();
  }
  if (have_disk) {
    graph_load_from_disk(); /* logs its own outcome; a fresh/empty disk
                                (or none at all) just means we start
                                from an empty graph -- nothing fatal */
  }

  const struct limine_file *initrd = boot_find_module("initrd.tar");
  if (initrd != NULL) {
    /* Always re-unpacks, even over a graph that gload'd a previous
     * session: '/bin' should always match the binaries THIS kernel
     * build actually shipped, while everything else the graph
     * restored (the user's own gtouch/gmk/sstring content) is left
     * completely untouched -- initrd_unpack() only ever writes the
     * specific paths it's packing, and per-file logs "overwrote a
     * pre-existing entry" when that's what happened. This is what
     * makes '/bin' part of the one real, persistent namespace instead
     * of a second, disconnected filesystem that only ever existed for
     * one boot -- gnodeinfo/gls/ggc all see it exactly like anything
     * else in the graph now. */
    initrd_unpack(initrd->address, initrd->size);
  } else {
    kprintf("[boot] no initrd module found -- '/bin' will be empty\n");
  }

  loom_boot(); /* strand scan + supervisor -- now that the graph
                   actually has content to scan, unlike at the
                   loom_init()-only point earlier in kmain() */

  if (strstr(g_boot.cmdline, "kshell") != NULL) {
    task_create("shell", shell_task, NULL);
  } else {
    struct task *nsh = process_spawn("/bin/nsh", "nsh", 0);
    if (nsh == NULL) {
      kprintf("[boot] couldn't spawn /bin/nsh -- falling back to the "
              "ring-0 kernel shell (see 'kshell' on the cmdline to "
              "always get this on purpose)\n");
      task_create("shell", shell_task, NULL);
    }
  }

  if (strstr(g_boot.cmdline, "selftest") != NULL) {
    task_create("selftest", selftest_task, NULL);
  }

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
  graph_init();

  /* GraphFS *is* "/" now -- see fs/graphfs_vfs.c's header comment.
   * It's empty at this exact point (nothing's touched the graph yet);
   * blockdev_init_task (below) is what actually populates it -- disk
   * bring-up needs the scheduler running first (task_block() on a
   * completion interrupt), so that can't happen any earlier than this
   * line no matter what. loom_init() just zeroes Loom's in-memory
   * strand table -- cheap, safe to do against an empty graph -- the
   * real strand scan (loom_boot()) waits for blockdev_init_task too,
   * for the same reason. */
  vfs_set_root(graphfs_vfs_root());
  loom_init();
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

  /* No "shell" task started directly here any more -- blockdev_init_task
   * spawns the interactive shell itself (nsh by default, or the ring-0
   * kernel shell with "kshell" on the cmdline) only once '/bin' is
   * guaranteed to actually exist. Same reasoning covers "selftest". */
  task_create("blockdev", blockdev_init_task, NULL);
  task_create("gautosave", graph_autosave_task, NULL);
  // task_create("heartbeat-a", heartbeat_task, "heartbeat-a");
  // task_create("heartbeat-b", heartbeat_task, "heartbeat-b");

  sched_enter_idle(); /* this CPU becomes idle/bsp; never returns */
}
