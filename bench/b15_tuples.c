#include <stdio.h>
#include <stdint.h>

typedef struct {
    uint64_t _0;
    uint64_t _1;
} tuple_u64_u64;

static inline tuple_u64_u64 lcg_step(uint64_t x, uint64_t y) {
    uint64_t nx = (x * 6364136223846793005ULL + 1442695040888963407ULL) & 9223372036854775807ULL;
    uint64_t ny = (y * 2862933555777941757ULL + 3037000493ULL) & 9223372036854775807ULL;
    tuple_u64_u64 res = { nx, ny };
    return res;
}

static inline tuple_u64_u64 mix(tuple_u64_u64 p) {
    uint64_t q = p._0 / 65536ULL;
    uint64_t r = p._1 % 65536ULL;
    tuple_u64_u64 res = { p._0 ^ r, (p._1 + q) & 9223372036854775807ULL };
    return res;
}

int main(void) {
    const int64_t ROUNDS = 20000000;
    uint64_t x = 123456789ULL;
    uint64_t y = 987654321ULL;
    uint64_t acc = 0;
    for (int64_t r = 0; r < ROUNDS; r++) {
        tuple_u64_u64 n = lcg_step(x, y);
        tuple_u64_u64 m = mix(n);
        x = m._0;
        y = m._1;
        acc = (acc + x + y) & 2147483647ULL;
    }
    printf("acc=%llu x=%llu y=%llu\n", (unsigned long long)acc, (unsigned long long)x, (unsigned long long)y);
    return 0;
}
