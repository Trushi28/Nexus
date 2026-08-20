#include "ulib.h"

/* Smoke test for split(): the child prints its own pid (proving it's
 * a genuinely different, independently-scheduled task) and the
 * message it was handed (proving it's running against its OWN copy
 * of this binary's .rodata -- the string literal below lives at the
 * same virtual address in both processes, but is backed by two
 * separate physical pages after the split). The parent's own pid
 * should differ from the child's, and both keep running afterward --
 * unlike exec(), nothing here replaces or ends the caller. */
static void worker(void *arg) {
  const char *label = (const char *)arg;
  char buf[16];

  u_print("[child ] pid ");
  u_itoa(u_getpid(), buf);
  u_print(buf);
  u_print(" -- ");
  u_print(label);
  u_print("\n");
}

int main(void) {
  char buf[16];

  u_print("[parent] pid ");
  u_itoa(u_getpid(), buf);
  u_print(buf);
  u_print(" splitting...\n");

  int pid = u_split(worker, "hello from the split image");
  if (pid < 0) {
    u_print("split failed\n");
    return 1;
  }

  u_print("[parent] child pid ");
  u_itoa(pid, buf);
  u_print(buf);
  u_print(", waiting...\n");

  int code = u_wait(pid);
  u_print("[parent] child exited with code ");
  u_itoa(code, buf);
  u_print(buf);
  u_print("\n");
  return 0;
}
