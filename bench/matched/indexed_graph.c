#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
// Inputs are validated by the benchmark manifest/harness.
static int64_t input(char **argv, int i) { return strtoll(argv[i], NULL, 10); }

typedef struct { int64_t next, value; } Node;
int main(int argc,char **argv) {
    int64_t rounds=input(argv,1),seed=input(argv,2),n=input(argv,3),cursor=seed%n,acc=0;
    Node nodes[4096];
    for(int64_t i=0;i<4096;++i) { nodes[i].next=(i*17+1)%n; nodes[i].value=(i+seed)&65535; }
    for(int64_t r=0;r<rounds;++r) {
        if((uint64_t)cursor>=4096) abort();
        cursor=nodes[cursor].next;
        if((uint64_t)cursor>=4096) abort();
        nodes[cursor].value=(nodes[cursor].value+acc+r)&65535;
        acc=(acc*31+nodes[cursor].value)&2147483647;
    }
    printf("%lld %lld\n",(long long)acc,(long long)cursor); return 0;
}
