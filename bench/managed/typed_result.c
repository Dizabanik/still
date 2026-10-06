#include "semantics_common.h"
typedef struct { uint64_t tag,value; } Result;
_Static_assert(sizeof(Result)==16,"result ABI");
static Result check(uint64_t value,uint64_t period) {
 return value%period ? (Result){0,value*17+3} : (Result){1,value};
}
static Result process(uint64_t value,uint64_t period) {
 Result r=check(value,period); if(r.tag) return r; return (Result){0,r.value+11};
}
int main(int argc,char **argv) {
 if(argc!=4) return 2;
 uint64_t rounds=input(argv[1]),seed=input(argv[2]),period=input(argv[3]);
 if(rounds>100000000 || seed>65535 || !period || period>256) return 2;
 uint64_t checksum=0,counted=0;
 for(uint64_t i=0;i<rounds;++i) {Result r=process(seed+i,period); checksum+=r.value; counted+=r.tag;}
 report(checksum,counted);
}
