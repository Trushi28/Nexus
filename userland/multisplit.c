#include "ulib.h"

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
