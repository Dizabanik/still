#include <stdio.h>
#include <stdint.h>

// C twin of b7_errors.kawa: the idiomatic error-code pattern. validate()
// returns 0 on success or fills a payload struct and returns a code -- what
// Kawa's filter/press/dregs lowers to, written out by hand.

#define N 200000000

typedef struct {
    int32_t code;
    int32_t pos;
} ParseErr;

static int32_t validate(uint64_t i, ParseErr *err) {
    uint32_t x = (uint32_t)(i * 2654435761u);
    if (x % 7u == 3u) {
        err->code = 1;
        err->pos = (int32_t)(x % 1000u);
        return 1;
    }
    if ((x & 0xFFu) == 0xFFu) {
        err->code = 2;
        err->pos = (int32_t)(x >> 8);
        return 2;
    }
    return 0;
}

int main(void) {
    int64_t ok_count = 0;
    int32_t acc = 0;
    for (int64_t i = 0; i < N; i++) {
        uint32_t x = (uint32_t)(i * 2654435761u);
        ParseErr e;
        int32_t rc = validate(i, &e);
        if (rc == 0) {
            acc += (int32_t)x;
            ok_count++;
        } else {
            acc -= e.code * e.pos;
        }
    }
    printf("ok=%d acc=%d\n", (int32_t)ok_count, acc);
    return 0;
}
