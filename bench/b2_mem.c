#include <stdio.h>
#include <stdint.h>

static uint32_t data[67108864];

int main(void) {
    for (int i = 0; i < 67108864; i++) {
        data[i] = (uint32_t)i * 2654435761u;
    }
    uint64_t sum = 0;
    for (int i = 0; i < 67108864; i++) {
        sum += data[i];
    }
    printf("sum=%llu\n", (unsigned long long)sum);
    return 0;
}
