#include <stdio.h>
#include <stdint.h>

typedef struct {
    int64_t x;
    int64_t y;
} Point;

typedef struct {
    int64_t first;
    int64_t second;
} Pair;

int main(void) {
    const int64_t ROUNDS = 50000000;
    int64_t acc = 123456789;

    for (int64_t r = 0; r < ROUNDS; r++) {
        Point pt = { (acc ^ r) & 65535, ((acc >> 16) ^ r) & 65535 };

        // 1. Named struct destructuring
        int64_t x = pt.x;
        int64_t y = pt.y;

        // 2. Renamed struct destructuring
        int64_t px = pt.x;
        int64_t py = pt.y;

        // 3. Inferred struct destructuring
        Point tmp = { px + 1, py + 2 };
        int64_t ix = tmp.x;
        int64_t iy = tmp.y;

        // 4. Positional struct destructuring
        Pair p = { x * 3 + iy, y * 5 + ix };
        int64_t p1 = p.first;
        int64_t p2 = p.second;

        // 5. Positional array destructuring
        int64_t arr[2];
        arr[0] = p1 ^ p2;
        arr[1] = p1 + p2;
        int64_t a0 = arr[0];
        int64_t a1 = arr[1];

        acc = (acc * 31 + a0 + a1) & 2147483647;
    }

    printf("acc=%lld\n", (long long)acc);
    return 0;
}
