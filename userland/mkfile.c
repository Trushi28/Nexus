#include "syscall.h"
#include "ulib.h"

/* Minimal smoke test for O_CREAT: writes a fixed line to
 * /tmp_test.txt, creating it if it doesn't exist yet (a top-level
 * path needs no intermediate directory, so this always lands
 * directly under "/"). 'cat /tmp_test.txt' afterward, in either
 * shell, is the real verification.
 *
 * Running this twice in a row produces byte-identical output, since
 * the message is fixed-length and there's still no O_TRUNC -- a
 * SECOND write of a SHORTER string would leave trailing bytes from
 * the first, longer write in place. Worth knowing; not something this
 * demo needs to handle. */
int main(void) {
  const char *path = "/tmp_test.txt";
  const char *msg = "hello from mkfile -- O_CREAT works\n";

  int fd = u_open(path, O_WRONLY | O_CREAT);
  if (fd < 0) {
    u_print("mkfile: open failed\n");
    return 1;
  }

  int n = u_write(fd, msg, u_strlen(msg));
  u_close(fd);

  if (n < 0) {
    u_print("mkfile: write failed\n");
    return 1;
  }

  char buf[16];
  u_itoa(n, buf);
  u_print("mkfile: wrote ");
  u_print(buf);
  u_print(" byte(s) to ");
  u_print(path);
  u_print("\n");
  return 0;
}
