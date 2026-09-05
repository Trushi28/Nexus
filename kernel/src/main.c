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

/*
 * PID-1 replacement for loomd -- see userland/init.c for the actual
 * service-supervision logic (reading "/services" out of GraphFS via
 * ordinary syscalls). This kernel-side task's only job is to keep
 * SOME /bin/init alive: if it dies (crash, or a bug), restart it
 * after a short delay rather than leaving the machine with nothing
 * driving it at all. Same crash-loop-then-give-up shape the old
 * direct-nsh-spawn fallback used when /bin/nsh was missing.
 */
#define INIT_RESTART_DELAY_MS 1000
#define INIT_MAX_RAPID_RESTARTS 5
#define INIT_RESTART_WINDOW_MS 5000

static void init_task(void *arg) {
  (void)arg;
  uint64_t restart_count = 0;
  uint64_t window_start = timer_uptime_ms();

  for (;;) {
    struct task *init = process_spawn("/bin/init", "init", 0);
    if (init == NULL) {
      kprintf("[boot] /bin/init not found -- falling back to the "
              "ring-0 kernel shell (see 'kshell' on the cmdline to "
              "always get this on purpose)\n");
      task_create("shell", shell_task, NULL);
      task_exit();
    }

    int code = process_wait(init);
    kprintf("[boot] /bin/init exited with code %d\n", code);

    uint64_t now = timer_uptime_ms();
    if (now - window_start < INIT_RESTART_WINDOW_MS) {
      restart_count++;
    } else {
      restart_count = 1;
      window_start = now;
    }
    if (restart_count > INIT_MAX_RAPID_RESTARTS) {
      kprintf("[boot] /bin/init crash-looped -- falling back to the "
              "ring-0 kernel shell instead of retrying forever\n");
      task_create("shell", shell_task, NULL);
      task_exit();
    }

    sched_sleep_ms(INIT_RESTART_DELAY_MS);
  }
}

/*
 * Seeds "/services/nsh" with sane defaults the very first time this
 * graph is ever populated -- mirrors typing gtouch/gwrite by hand at
 * the kernel shell, just done once in C so a fresh boot still reaches
 * an interactive shell with no manual setup required. Skipped
 * whenever "services" already resolves to something -- either a
 * graph_load_from_disk() snapshot from a previous session, or a
 * service someone already defined this session -- so this never
 * clobbers real state.
 */
static void seed_default_services(void) {
  if (sstring_get("services") != NULL) {
    return;
  }

  bool created;
  struct gnode *path = graph_touch("services/nsh/path", &created);
  if (path != NULL) {
    graph_write(path, 0, "/bin/nsh", 8);
  }
  struct gnode *respawn = graph_touch("services/nsh/respawn", &created);
  if (respawn != NULL) {
    graph_write(respawn, 0, "always", 6);
  }
  struct gnode *uid = graph_touch("services/nsh/uid", &created);
  if (uid != NULL) {
    graph_write(uid, 0, "0", 1);
  }
  kprintf("[boot] seeded default service '/services/nsh' (fresh graph)\n");
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

  const struct limine_file *initrd = boot_find_module("initrd.tar");
  if (initrd != NULL) {
    initrd_unpack(initrd->address, initrd->size);
  } else {
    kprintf("[boot] no initrd module found -- '/bin' will be empty\n");
  }

  if (strstr(g_boot.cmdline, "kshell") != NULL) {
    task_create("shell", shell_task, NULL);
  } else {
    seed_default_services();
    task_create("init", init_task, NULL);
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

  isr_init();

  cpu_enable_nx();

  fb_init();
  console_init();

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

  vfs_set_root(graphfs_vfs_root());
  /* Ephemeral, non-persisted state -- doesn't ride along with
   * GraphFS's whole-graph snapshot the way anything under the graphfs
   * root does, which is exactly the point: scratch files and runtime
   * sockets/pidfiles shouldn't survive a reboot or bloat gsync's
   * payload. vfs_mount() grafts these in without requiring "tmp"/
   * "run" to exist as graph nodes -- see fs/vfs.c's find_mount(). */
  vfs_mount("/tmp", tmpfs_create_root());
  vfs_mount("/run", tmpfs_create_root());
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

  task_create("blockdev", blockdev_init_task, NULL);
  task_create("gautosave", graph_autosave_task, NULL);
  // task_create("heartbeat-a", heartbeat_task, "heartbeat-a");
  // task_create("heartbeat-b", heartbeat_task, "heartbeat-b");

  sched_enter_idle(); /* this CPU becomes idle/bsp; never returns */
}
