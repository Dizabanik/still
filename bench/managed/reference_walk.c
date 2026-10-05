/* Same source runtime, allocation policy, initialized values and bounds.
 * Whole-process timings include initialization and cleanup in both languages.
 * This measures lowering efficiency at equal safety, not a raw-pointer C race. */
#include "../../src/runtime/wky_memory.c"
#include <inttypes.h>
#include <errno.h>

static int64_t add(int64_t a,int64_t b) {
    int64_t result;
    if (__builtin_add_overflow(a,b,&result)) abort();
    return result;
}
static int64_t sub(int64_t a,int64_t b) {
    int64_t result;
    if (__builtin_sub_overflow(a,b,&result)) abort();
    return result;
}
static int64_t mul(int64_t a,int64_t b) {
    int64_t result;
    if (__builtin_mul_overflow(a,b,&result)) abort();
    return result;
}

static int64_t input(const char *text) {
    char *end;
    errno=0;
    unsigned long long n=strtoull(text,&end,10);
    if (errno || *end || *text=='-' || n>INT64_MAX) exit(2);
    return (int64_t)n;
}
static int64_t walk(WkyRef p, int64_t rounds, int64_t seed, int64_t mask, int scatter, int stable) {
    int64_t total=0;
    int64_t *data=stable ? __wky_mem_pin(&p) : NULL;
    for (int64_t i=0;i<rounds;i=add(i,1)) {
        int64_t index=(scatter ? add(mul(i,1664525),seed) : add(i,seed))&mask;
        if (stable) {
            if ((uint64_t)index>=p.extent/sizeof(int64_t)) abort();
            total=add(total,data[index]);
        } else {
            int64_t *element=__wky_mem_address(p.descriptor,p.generation,p.offset,p.extent,
                                               (uint64_t)index,sizeof(int64_t));
            total=add(total,*element);
        }
    }
    if (stable) __wky_mem_unpin(&p);
    return total;
}
int main(int argc,char **argv) {
    if (argc!=6) return 2;
    int64_t rounds=input(argv[1]),seed=input(argv[2]),size=input(argv[3]);
    int64_t mode=input(argv[4]),scatter=input(argv[5]);
    if (rounds>1000000000 || seed>65535 || size<1 || size>1048576 || (size&(size-1)) || mode>1 || scatter>1) return 2;
    WkyRef a;
    if (!__wky_mem_alloc(&a,(uint64_t)size,sizeof(int64_t))) abort();
    int64_t *data=__wky_mem_pin(&a);
    for (int64_t i=0;i<size;i=add(i,1)) data[i]=add(mul(i,17),seed);
    __wky_mem_unpin(&a);
    uint64_t bc=__wky_mem_metric(7),bp=__wky_mem_metric(8);
    int64_t total=walk(a,rounds,seed,sub(size,1),(int)scatter,(int)mode);
    uint64_t ac=__wky_mem_metric(7),ap=__wky_mem_metric(8);
    __wky_mem_drop(&a);
    printf("%"PRId64" %"PRIu64" %"PRIu64" %"PRIu64" %"PRIu64" %"PRIu64" %"PRIu64" %"PRIu64" %"PRIu64"\n",
           total,__wky_mem_metric(0),__wky_mem_metric(1),__wky_mem_metric(2),bc,ac,bp,ap,__wky_mem_metric(4));
}
