#include <stdio.h>
#include <stdint.h>

static inline uint32_t fnv1a(uint32_t h, uint32_t v) {
    return (h ^ v) * 16777619u;
}

int main(void) {
    const long long ROUNDS = 200000000;
    uint32_t h = 2166136261u;
    for (long long r = 0; r < ROUNDS; r++) {
        h = fnv1a(h, (uint32_t)r);
    }
    printf("h=%u\n", h);
    return 0;
}
