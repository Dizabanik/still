#include <stdio.h>
#include <stdint.h>

typedef struct {
    int64_t *data;
    int64_t len;
} Slice;

static inline int64_t sum_slice(Slice s) {
    int64_t total = 0;
    for (int64_t i = 0; i < s.len; i++) {
        total += s.data[i];
    }
    return total;
}

static inline void transform_slice(Slice s, int64_t delta) {
    for (int64_t i = 0; i < s.len; i++) {
        s.data[i] = (s.data[i] * 3 + delta) & 65535;
    }
}

int main(void) {
    const int64_t ROUNDS = 50000;
    int64_t buf[1024];
    for (int64_t i = 0; i < 1024; i++) {
        buf[i] = (i * 17 + 5) & 65535;
    }

    int64_t acc = 0;
    for (int64_t r = 0; r < ROUNDS; r++) {
        int64_t start = (r * 7) & 511;
        int64_t len = ((r * 13) & 255) + 32;

        Slice view = { &buf[start], len };
        int64_t s = sum_slice(view);
        transform_slice(view, r & 7);

        acc = (acc * 37 + s) & 2147483647;
    }

    printf("acc=%lld\n", (long long)acc);
    return 0;
}
