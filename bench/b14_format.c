#include <stdio.h>
#include <stdint.h>

int main(void) {
    const int64_t N = 1000000;
    int64_t acc = 0;
    for (int64_t i = 0; i < N; i++) {
        acc = (acc * 31 + i) & 1048575;
        double val = (double)(i % 1000) * 0.001;
        printf("i=%lld acc=%lld val=%.2f\n", (long long)i, (long long)acc, val);
    }
    return 0;
}
