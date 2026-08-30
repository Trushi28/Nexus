#include "syscall.h"
#include "ulib.h"

int main(void) {
  const char *path = "/tmp_test.txt";
  const char *msg = "hello from mkfile -- O_CREAT/O_TRUNC works\n";

  int fd = u_open(path, O_WRONLY | O_CREAT | O_TRUNC);
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
