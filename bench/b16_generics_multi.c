#include <stdio.h>
#include <stdint.h>

typedef struct {
    int64_t first;
    int32_t second;
} Pair_i64_i32;

typedef struct {
    int32_t first;
    int64_t second;
} Pair_i32_i64;

static inline Pair_i64_i32 Pair_i64_i32_new(int64_t f, int32_t s) {
    Pair_i64_i32 p = { f, s };
    return p;
}

static inline int64_t Pair_i64_i32_get_first(Pair_i64_i32 self) {
    return self.first;
}

static inline int32_t Pair_i64_i32_get_second(Pair_i64_i32 self) {
    return self.second;
}

static inline Pair_i32_i64 Pair_i64_i32_swap(Pair_i64_i32 self) {
    Pair_i32_i64 p = { self.second, self.first };
    return p;
}

static inline Pair_i64_i32 Pair_i32_i64_swap(Pair_i32_i64 self) {
    Pair_i64_i32 p = { self.second, self.first };
    return p;
}

int main(void) {
    const int64_t ROUNDS = 20000000;
    Pair_i64_i32 p = Pair_i64_i32_new(123456789LL, 42);
    int64_t acc = 0;

    for (int64_t r = 0; r < ROUNDS; r++) {
        int64_t a = Pair_i64_i32_get_first(p);
        int32_t b = Pair_i64_i32_get_second(p);
        int64_t na = (a ^ (r * 16777619LL)) & 2147483647LL;
        int32_t nb = (b * 31 + (int32_t)(r & 255)) ^ (int32_t)(a & 65535);
        p = Pair_i32_i64_swap(Pair_i64_i32_swap(Pair_i64_i32_new(na, nb)));
        acc = (acc + Pair_i64_i32_get_first(p) + (int64_t)Pair_i64_i32_get_second(p)) & 2147483647LL;
    }

    printf("acc=%lld a=%lld b=%d\n", (long long)acc, (long long)Pair_i64_i32_get_first(p), Pair_i64_i32_get_second(p));
    return 0;
}
