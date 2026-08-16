#include "ulib.h"
#include "syscall.h"
#include <stdbool.h>

size_t u_strlen(const char *s) {
  size_t n = 0;
  while (s[n]) {
    n++;
  }
  return n;
}

void u_print(const char *s) { SC3(SYS_write, STDOUT_FILENO, s, u_strlen(s)); }

void u_putc(char c) { SC3(SYS_write, STDOUT_FILENO, &c, 1); }

int u_read_line(char *buf, size_t max) {
  if (max == 0) {
    return 0;
  }
  long n = SC3(SYS_read, STDIN_FILENO, buf, max - 1);
  if (n < 0) {
    n = 0;
  }
  if (n > 0 && buf[n - 1] == '\n') {
    n--;
  }
  buf[n] = '\0';
  return (int)n;
}

void u_itoa(int val, char *buf) {
  char tmp[12];
  int n = 0;
  bool neg = val < 0;
  unsigned uval = neg ? (unsigned)(-(val + 1)) + 1u : (unsigned)val;

  if (uval == 0) {
    tmp[n++] = '0';
  }
  while (uval > 0) {
    tmp[n++] = (char)('0' + (uval % 10));
    uval /= 10;
  }

  int i = 0;
  if (neg) {
    buf[i++] = '-';
  }
  while (n > 0) {
    buf[i++] = tmp[--n];
  }
  buf[i] = '\0';
}

int u_atoi(const char *s) {
  int v = 0;
  int neg = 0;
  if (*s == '-') {
    neg = 1;
    s++;
  }
  while (*s >= '0' && *s <= '9') {
    v = v * 10 + (*s - '0');
    s++;
  }
  return neg ? -v : v;
}

int u_getpid(void) { return (int)SC0(SYS_getpid); }

void u_sleep_ms(unsigned ms) { SC1(SYS_sleep_ms, ms); }

unsigned u_uptime_ms(void) { return (unsigned)SC0(SYS_uptime_ms); }
int u_strcmp(const char *a, const char *b) {
  while (*a && (*a == *b)) {
    a++;
    b++;
  }
  return (unsigned char)*a - (unsigned char)*b;
}

char *u_strncpy(char *dst, const char *src, size_t n) {
  size_t i = 0;
  for (; i < n && src[i]; i++) {
    dst[i] = src[i];
  }
  for (; i < n; i++) {
    dst[i] = '\0';
  }
  return dst;
}

int u_spawn(const char *path) { return (int)SC1(SYS_spawn, path); }

int u_wait(int pid) { return (int)SC1(SYS_wait, pid); }

int u_open(const char *path, int flags) {
  return (int)SC2(SYS_open, path, flags);
}

int u_read(int fd, void *buf, size_t len) {
  return (int)SC3(SYS_read, fd, buf, len);
}

int u_close(int fd) { return (int)SC1(SYS_close, fd); }

bool u_readdir(const char *path, unsigned index, char *name_out,
               size_t name_max) {
  long r = SC4(SYS_readdir, path, index, name_out, name_max);
  return r == 0;
}

int u_kill(int pid) { return (int)SC1(SYS_kill, pid); }

bool u_find_task(int pid, nx_task_info_t *out) {
  unsigned idx = 0;
  nx_task_info_t info;
  while (u_ps(idx, &info)) {
    if ((int)info.pid == pid) {
      *out = info;
      return true;
    }
    idx++;
  }
  return false;
}

bool u_ps(unsigned index, nx_task_info_t *out) {
  long r = SC2(SYS_ps, index, out);
  return r == 0;
}
