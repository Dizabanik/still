#include <stdio.h>
#include <stdint.h>

int main(void) {
    const int64_t ROUNDS = 50000;
    int64_t buf[1024];
    for (int64_t i = 0; i < 1024; i++) {
        buf[i] = (i * 19 + 7) & 65535;
    }

    int64_t acc = 0;
    for (int64_t r = 0; r < ROUNDS; r++) {
        for (int64_t i = 0; i < 1024; i++) {
            buf[i] = (buf[i] * 3 + (r & 15)) & 65535;
            acc = (acc + buf[i]) & 2147483647;
        }
        acc = (acc ^ buf[0] ^ buf[1023]) & 2147483647;
    }

    printf("acc=%lld\n", (long long)acc);
    return 0;
}
