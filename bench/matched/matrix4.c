#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
// Inputs are validated by the benchmark manifest/harness.
static int64_t input(char **argv, int i) { return strtoll(argv[i], NULL, 10); }

int main(int argc,char **argv) {
    int64_t rounds=input(argv,1),seed=input(argv,2),m[16],v[4],acc=0;
    for(int64_t i=0;i<16;++i) m[i]=(i*7+seed)&15;
    for(int64_t i=0;i<4;++i) v[i]=(seed+i)&65535;
    for(int64_t r=0;r<rounds;++r) { int64_t out[4];
        for(int64_t row=0;row<4;++row) { int64_t sum=0;
            for(int64_t col=0;col<4;++col) sum+=m[row*4+col]*v[col];
            out[row]=(sum+r)&65535;
        }
        for(int64_t i=0;i<4;++i) { v[i]=out[i]; acc=(acc*31+v[i])&2147483647; }
    }
    printf("%lld %lld %lld\n",(long long)acc,(long long)v[0],(long long)v[3]); return 0;
}
