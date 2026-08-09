#include "ulib.h"

static unsigned rng_state;

static unsigned next_rand(void) {
    rng_state = rng_state * 1103515245u + 12345u;
    return (rng_state >> 16) & 0x7fffu;
}

int main(void) {
    rng_state = u_uptime_ms() * 2654435761u | 1u;
    int target = 1 + (int)(next_rand() % 100);
    int tries = 0;

    u_print("Guess a number between 1 and 100.\n");

    char line[16];
    for (;;) {
        tries++;
        u_print("> ");
        u_read_line(line, sizeof(line));
        int guess = u_atoi(line);

        if (guess == target) {
            u_print("Correct! ");
            char buf[16];
            u_itoa(tries, buf);
            u_print(buf);
            u_print(tries == 1 ? " try.\n" : " tries.\n");
            break;
        }
        u_print(guess < target ? "Higher.\n" : "Lower.\n");
    }

    return 0;
}
