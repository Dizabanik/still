#include <stdio.h>
#include <stdint.h>

typedef struct {
    int64_t value;
} Box_i64;

static inline Box_i64 Box_i64_new(int64_t v) {
    Box_i64 b = { v };
    return b;
}

static inline int64_t Box_i64_get(Box_i64 self) {
    return self.value;
}

static inline void Box_i64_put(Box_i64 *self, int64_t v) {
    self->value = v;
}

typedef struct {
    int32_t value;
} Box_i32;

static inline Box_i32 Box_i32_new(int32_t v) {
    Box_i32 b = { v };
    return b;
}

static inline int32_t Box_i32_get(Box_i32 self) {
    return self.value;
}

static inline void Box_i32_put(Box_i32 *self, int32_t v) {
    self->value = v;
}

int main(void) {
    const int64_t ROUNDS = 50000000;
    Box_i64 b_i = Box_i64_new(1);
    Box_i32 b_w = Box_i32_new(1);

    for (int64_t r = 0; r < ROUNDS; r++) {
        int64_t v = Box_i64_get(b_i);
        int64_t next_i = (v ^ (r * 16777619LL)) & 2147483647LL;
        Box_i64_put(&b_i, next_i);

        int32_t vw = Box_i32_get(b_w);
        int32_t next_w = (int32_t)((uint32_t)vw * 31U + (uint32_t)(r & 255)) ^ (int32_t)(v & 65535);
        Box_i32_put(&b_w, next_w);
    }

    printf("i=%lld w=%d\n", (long long)Box_i64_get(b_i), Box_i32_get(b_w));
    return 0;
}
