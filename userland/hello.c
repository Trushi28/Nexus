#include "ulib.h"

int main(void) {
    u_print("Hello from ring 3, Nexus!\n");

    char buf[16];
    u_itoa(u_getpid(), buf);
    u_print("my pid is ");
    u_print(buf);
    u_print("\n");

    return 0;
}
