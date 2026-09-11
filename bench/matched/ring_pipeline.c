#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
// Inputs are validated by the benchmark manifest/harness.
static int64_t input(char **argv, int i) { return strtoll(argv[i], NULL, 10); }

int main(int argc,char **argv) {
    int64_t rounds=input(argv,1),seed=input(argv,2),a[64],b[64],c[64],sent=0,acc=0;
    while(sent<rounds) {
        int64_t count=rounds-sent; if(count>64) count=64;
        for(int64_t i=0;i<count;++i) a[i]=((sent+i+seed)*3+1)&2147483647;
        for(int64_t i=0;i<count;++i) b[i]=(a[i]*3+1)&2147483647;
        for(int64_t i=0;i<count;++i) c[i]=(b[i]*3+1)&2147483647;
        for(int64_t i=0;i<count;++i) acc=(acc+c[i])&2147483647;
        sent+=count;
    }
    printf("%lld\n",(long long)acc); return 0;
}
