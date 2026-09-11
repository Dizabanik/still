#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
// Inputs are validated by the benchmark manifest/harness.
static int64_t input(char **argv, int i) { return strtoll(argv[i], NULL, 10); }

int main(int argc,char **argv) {
    int64_t rounds=input(argv,1), state=input(argv,2), n=input(argv,3), data[4096], acc=0;
    for (int64_t i=0;i<4096;++i) data[i]=(i*17+state)&65535;
    for (int64_t r=0;r<rounds;++r) {
        state=(state*1103515245+12345)&2147483647;
        int64_t index=state%n;
        if ((uint64_t)index >= (uint64_t)n) abort();
        data[index]=(data[index]*3+r)&65535;
        acc=(acc+data[index])&2147483647;
    }
    printf("%lld %lld\n",(long long)acc,(long long)state); return 0;
}
