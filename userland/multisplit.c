#include "ulib.h"

/* Exercises wait_any(): splits three children with staggered sleeps,
 * then reaps them via u_wait_any() in whatever order they ACTUALLY
 * finish (shortest sleep first), not creation order -- proving it's
 * really "whoever's done next," not secretly "child 1, then 2, then
 * 3." A final call after all three are gone should report there's
 * nothing left. `ps` while this is running (from the other shell, or
 * quickly from this one before they all finish) is also a good way to
 * see the name-tiebreak in action: each child shows up as
 * "<this path>~<pid>", not three identical entries. */

struct worker_spec {
  unsigned sleep_ms;
};

static void worker(void *arg) {
  struct worker_spec *spec = (struct worker_spec *)arg;
  u_sleep_ms(spec->sleep_ms);
  char buf[16];
  u_print("[child ] pid ");
  u_itoa(u_getpid(), buf);
  u_print(buf);
  u_print(" done\n");
  /* Returning here (rather than calling anything exit-like directly)
   * is the intended way to end a split() child -- _split_trampoline
   * (crt0.S) converts an ordinary return into exit(0) automatically,
   * the same safety net crt0's own _start gives main(). */
}

int main(void) {
  char buf[16];
  static struct worker_spec specs[3] = {{300}, {100}, {200}};

  u_print("splitting 3 children with staggered sleeps (300/100/200ms)...\n");

  for (int i = 0; i < 3; i++) {
    int pid = u_split(worker, &specs[i]);
    if (pid < 0) {
      u_print("split failed\n");
      return 1;
    }
    u_print("  spawned pid ");
    u_itoa(pid, buf);
    u_print(buf);
    u_print("\n");
  }

  u_print("reaping via wait_any, in completion order:\n");
  for (int i = 0; i < 3; i++) {
    int code = -999;
    int pid = u_wait_any(&code);
    u_print("  wait_any -> pid ");
    u_itoa(pid, buf);
    u_print(buf);
    u_print(", code ");
    u_itoa(code, buf);
    u_print(buf);
    u_print("\n");
  }

  int code = -999;
  int pid = u_wait_any(&code);
  u_print("wait_any once more -> ");
  u_itoa(pid, buf);
  u_print(buf);
  u_print(" (expect -1: nothing left)\n");

  return 0;
}
