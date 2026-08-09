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

void u_print(const char *s) {
    SC3(SYS_write, STDOUT_FILENO, s, u_strlen(s));
}

void u_putc(char c) {
    SC3(SYS_write, STDOUT_FILENO, &c, 1);
}

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

int u_getpid(void) {
    return (int)SC0(SYS_getpid);
}

void u_sleep_ms(unsigned ms) {
    SC1(SYS_sleep_ms, ms);
}

unsigned u_uptime_ms(void) {
    return (unsigned)SC0(SYS_uptime_ms);
}
