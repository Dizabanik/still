#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
// Inputs are validated by the benchmark manifest/harness.
static int64_t input(char **argv, int i) { return strtoll(argv[i], NULL, 10); }

int main(int argc,char **argv) {
    int64_t rounds=input(argv,1),seed=input(argv,2),n=input(argv,3),data[4096],acc=0;
    for(int64_t i=0;i<4096;++i) data[i]=(i+seed)&65535;
    int64_t *left=data,*right=data+1;
    for(int64_t r=0;r<rounds;++r) {
        int64_t i=r%(n-1);
        if((uint64_t)i >= (uint64_t)(n-1)) abort();
        right[i]=(left[i]*3+right[i]+r)&65535;
        acc=(acc+right[i])&2147483647;
    }
    printf("%lld %lld\n",(long long)acc,(long long)data[n-1]); return 0;
}
