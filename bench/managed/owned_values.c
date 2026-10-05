#include "../../src/runtime/wky_memory.c"
#include <errno.h>
#include <inttypes.h>
typedef struct { uint64_t tag; WkyRef buffers[4]; } Bag;
static const WkyOwnedField buffer_fields[]={{0,4,sizeof(WkyRef),NULL}};
static const WkyValueLayout buffer_layout={4*sizeof(WkyRef),1,buffer_fields};
static const WkyOwnedField fields[]={{offsetof(Bag,buffers),1,4*sizeof(WkyRef),&buffer_layout}};
static const WkyValueLayout layout={sizeof(Bag),1,fields};
_Static_assert(sizeof(Bag)==136,"Whisky/C aggregate ABI");
static uint64_t input(const char *text) {
    char *end;
    errno=0;
    unsigned long long result=strtoull(text,&end,10);
    if (errno || !*text || *end || *text=='-') exit(2);
    return result;
}
static uint64_t *at(WkyRef r,uint64_t i) {
    return __wky_mem_address(r.descriptor,r.generation,r.offset,r.extent,i,8);
}
static Bag *bag(WkyRef r) {
    return __wky_mem_address(r.descriptor,r.generation,r.offset,r.extent,0,sizeof(Bag));
}
int main(int argc,char **argv) {
    if (argc!=5) return 2;
    uint64_t size=input(argv[1]), trials=input(argv[2]), seed=input(argv[3]), shape=input(argv[4]);
    if (size>1000000 || trials<1 || trials>100 || seed>65535 || shape>1) return 2;
    uint64_t originals=0,copies=0;
    for (uint64_t trial=0; trial<trials; ++trial) {
        Bag source={.tag=seed}, copied={0}, moved={0}, incoming;
        for (uint64_t buffer=0; buffer<4; ++buffer)
            if (!__wky_mem_alloc(&source.buffers[buffer],size,8)) abort();
        for (uint64_t buffer=0; buffer<4; ++buffer)
            for (uint64_t i=0; i<size; ++i) *at(source.buffers[buffer],i)=i*17+seed+buffer*23;
        if (shape==0) {
            if (!__wky_mem_value_clone(&incoming,&source,&layout,NULL)) abort();
            __wky_mem_value_store(&copied,&incoming,&layout,NULL);
            __wky_mem_value_take(&incoming,&source,&layout,NULL);
            __wky_mem_value_store(&moved,&incoming,&layout,NULL);
        } else {
            WkyRef heap;
            if (!__wky_mem_alloc(&heap,1,sizeof(Bag))) abort();
            Bag *slot=bag(heap);
            __wky_mem_value_take(&incoming,&source,&layout,NULL);
            __wky_mem_value_store(slot,&incoming,&layout,&heap);
            if (!__wky_mem_value_clone(&incoming,bag(heap),&layout,&heap)) abort();
            __wky_mem_value_store(&copied,&incoming,&layout,NULL);
            __wky_mem_value_take(&incoming,bag(heap),&layout,&heap);
            __wky_mem_value_store(&moved,&incoming,&layout,NULL);
            __wky_mem_drop(&heap);
        }
        if (size) {
            uint64_t *slot=at(copied.buffers[0],0), value=*at(copied.buffers[0],0)+1;
            *slot=value;
        }
        originals+=moved.tag; copies+=copied.tag;
        for (uint64_t buffer=0; buffer<4; ++buffer)
            for (uint64_t i=0; i<size; ++i) {
                originals+=*at(moved.buffers[buffer],i);
                copies+=*at(copied.buffers[buffer],i);
            }
        __wky_mem_value_drop(&moved,&layout);
        __wky_mem_value_drop(&copied,&layout);
        __wky_mem_value_drop(&source,&layout);
    }
    printf("%"PRIu64" %"PRIu64" %"PRIu64" %"PRIu64" %"PRIu64" %"PRIu64" %"PRIu64" %"PRIu64" %"PRIu64"\n",
        originals,copies,__wky_mem_metric(0),__wky_mem_metric(1),__wky_mem_metric(2),
        __wky_mem_metric(3),__wky_mem_metric(5),__wky_mem_metric(4),__wky_mem_metric(7));
    return 0;
}
