/* The same runtime, 32-byte row layout, two bounded views per iteration,
 * arithmetic checks and nesting of stable guards as the Kawa workload. */
#include "../../src/runtime/kawa_memory.c"
#include <inttypes.h>
#include <errno.h>
typedef struct { int64_t tag,samples[3]; } Row;
_Static_assert(sizeof(Row)==32,"benchmark layout");
static int64_t add(int64_t a,int64_t b) {
    int64_t result;
    if (__builtin_add_overflow(a,b,&result)) abort();
    return result;
}
static int64_t mul(int64_t a,int64_t b) {
    int64_t result;
    if (__builtin_mul_overflow(a,b,&result)) abort();
    return result;
}
static int64_t input(const char *text) {
    char *end; errno=0;
    unsigned long long n=strtoull(text,&end,10);
    if (errno || *end || *text=='-' || n>INT64_MAX) exit(2);
    return (int64_t)n;
}
static void *at(KawaRef r,uint64_t index,uint64_t size) {
    return __kawa_mem_address(r.descriptor,r.generation,r.offset,r.extent,index,size);
}
static int64_t walk(KawaRef rows,int64_t rounds,int64_t seed,int64_t mask,int scatter,int stable) {
    int64_t total=0;
    Row *data=stable ? __kawa_mem_pin(&rows) : NULL;
    for (int64_t i=0; i<rounds; i=add(i,1)) {
        int64_t index=(scatter ? add(mul(i,1664525),seed) : add(i,seed))&mask;
        Row *row;
        if (stable) {
            if ((uint64_t)index>=rows.extent/sizeof(Row)) abort();
            row=data+index;
        } else row=at(rows,(uint64_t)index,sizeof(Row));
        KawaRef field,samples;
        __kawa_mem_view(&field,&rows,&row->tag,sizeof(row->tag));
        if (stable) {
            if ((uint64_t)index>=rows.extent/sizeof(Row)) abort();
            row=data+index;
        } else row=at(rows,(uint64_t)index,sizeof(Row));
        __kawa_mem_view(&samples,&rows,row->samples,sizeof(row->samples));
        int64_t left,right;
        if (stable) {
            int64_t *field_data=__kawa_mem_pin(&field);
            int64_t *sample_data=__kawa_mem_pin(&samples);
            if (field.extent/8<=0 || samples.extent/8<=1) abort();
            left=field_data[0]; right=sample_data[1];
            __kawa_mem_unpin(&samples); __kawa_mem_unpin(&field);
        } else {
            left=*(int64_t *)at(field,0,8);
            right=*(int64_t *)at(samples,1,8);
        }
        total=add(total,add(left,right));
    }
    if (stable) __kawa_mem_unpin(&rows);
    return total;
}
int main(int argc,char **argv) {
    if (argc!=6) return 2;
    int64_t rounds=input(argv[1]),seed=input(argv[2]),size=input(argv[3]);
    int64_t mode=input(argv[4]),scatter=input(argv[5]);
    if (rounds>1000000000 || seed>65535 || size<1 || size>1048576 || (size&(size-1)) || mode>1 || scatter>1) return 2;
    KawaRef owned;
    if (!__kawa_mem_alloc(&owned,(uint64_t)size,sizeof(Row))) abort();
    Row *data=__kawa_mem_pin(&owned);
    for (int64_t i=0; i<size; i=add(i,1)) {
        data[i].tag=add(mul(i,17),seed);
        for (int64_t j=0; j<3; j=add(j,1)) data[i].samples[j]=add(add(mul(i,19),seed),mul(j,3));
    }
    __kawa_mem_unpin(&owned);
    uint64_t bc=__kawa_mem_metric(7),bp=__kawa_mem_metric(8);
    int64_t total=walk(owned,rounds,seed,size-1,(int)scatter,(int)mode);
    uint64_t ac=__kawa_mem_metric(7),ap=__kawa_mem_metric(8);
    __kawa_mem_drop(&owned);
    printf("%"PRId64" %"PRIu64" %"PRIu64" %"PRIu64" %"PRIu64" %"PRIu64" %"PRIu64" %"PRIu64" %"PRIu64" %"PRIu64"\n",
        total,__kawa_mem_metric(0),__kawa_mem_metric(1),__kawa_mem_metric(2),bc,ac,bp,ap,
        __kawa_mem_metric(4),__kawa_mem_metric(11));
}
