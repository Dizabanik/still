#include <stdio.h>
#include <stdint.h>

#define MAT4_DIM 4
#define MAT4_SIZE 16

int main(void) {
    const int64_t ROUNDS = 20000000;
    int64_t m[MAT4_SIZE];
    for (int64_t i = 0; i < MAT4_SIZE; i++) {
        m[i] = (i * 7 + 3) & 15;
    }

    int64_t v[MAT4_DIM];
    v[0] = 1;
    v[1] = 2;
    v[2] = 3;
    v[3] = 4;

    int64_t acc = 0;
    for (int64_t r = 0; r < ROUNDS; r++) {
        int64_t out[MAT4_DIM];
        for (int64_t row = 0; row < MAT4_DIM; row++) {
            int64_t sum = 0;
            for (int64_t col = 0; col < MAT4_DIM; col++) {
                sum += m[row * MAT4_DIM + col] * v[col];
            }
            out[row] = sum;
        }
        v[0] = (out[0] ^ r) & 65535;
        v[1] = out[1] & 65535;
        v[2] = (out[2] + 1) & 65535;
        v[3] = (out[3] ^ (r >> 2)) & 65535;
        acc = (acc * 31 + v[0] + v[1] + v[2] + v[3]) & 2147483647;
    }

    printf("acc=%lld\n", (long long)acc);
    return 0;
}
