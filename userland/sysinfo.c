#include "ulib.h"

int main(void) {
  u_print("   /\\_/\\\n"
          "  ( o.o )   Nexus userland\n"
          "   > ^ <\n"
          "  -------\n");

  char buf[16];

  u_print("  pid    : ");
  u_itoa(u_getpid(), buf);
  u_print(buf);
  u_print("\n");

  u_print("  uptime : ");
  u_itoa((int)u_uptime_ms(), buf);
  u_print(buf);
  u_print(" ms since boot\n");

  return 0;
}
