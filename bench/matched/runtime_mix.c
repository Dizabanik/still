#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
// Inputs are validated by the benchmark manifest/harness.
static int64_t input(char **argv, int i) { return strtoll(argv[i], NULL, 10); }

int main(int argc, char **argv) {
    int64_t rounds = input(argv,1), h = input(argv,2);
    for (int64_t r=0; r<rounds; ++r) h = ((h*31)^r)&2147483647;
    printf("%lld\n", (long long)h); return 0;
}
