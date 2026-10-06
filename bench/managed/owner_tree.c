/* Same runtime, checked accesses, zero initialization, transfer, deep clone,
 * and lexical drop order as Whisky. All arithmetic is explicitly uint64_t. */
#include "../../src/runtime/wky_memory.c"
#include <inttypes.h>
#include <errno.h>

typedef struct { uint64_t value; WkyRef child; } Node;
_Static_assert(sizeof(Node)==40 && sizeof(WkyRef)==32,"benchmark ABI");
static uint64_t input(const char *text) {
    char *end;
    errno=0;
    unsigned long long n=strtoull(text,&end,10);
    if (errno || end==text || *end || *text=='-') exit(2);
    return n;
}
static Node *node(WkyRef r) {
    return __wky_mem_address(r.descriptor,r.generation,r.offset,r.extent,0,sizeof(Node));
}
static WkyRef *owner_at(WkyRef r,uint64_t i) {
    return __wky_mem_address(r.descriptor,r.generation,r.offset,r.extent,i,sizeof(WkyRef));
}
static uint64_t *word(WkyRef r) {
    return __wky_mem_address(r.descriptor,r.generation,r.offset,r.extent,0,8);
}
static uint64_t chain_sum(WkyRef current) {
    uint64_t sum=0;
    while (current.descriptor && current.extent/sizeof(Node)) {
        sum+=node(current)->value;
        current=node(current)->child;
    }
    return sum;
}
static uint64_t fanout_sum(WkyRef current) {
    uint64_t sum=0;
    for (uint64_t i=0; i<current.extent/sizeof(WkyRef); ++i)
        sum+=*word(*owner_at(current,i));
    return sum;
}
int main(int argc,char **argv) {
    if (argc!=5) return 2;
    uint64_t size=input(argv[1]),trials=input(argv[2]),seed=input(argv[3]),shape=input(argv[4]);
    if (size>1000000 || trials<1 || trials>100 || seed>65535 || shape>1) return 2;
    uint64_t originals=0,copies=0;
    for (uint64_t trial=0; trial<trials; ++trial) {
        WkyRef root,copy;
        if (!shape) {
            if (!__wky_mem_alloc(&root,size ? 1 : 0,sizeof(Node))) abort();
            if (size) {
                Node *p=node(root);
                *(uint64_t *)__wky_mem_write_address(&root,&p->value,8)=seed;
            }
            for (uint64_t i=1; i<size; ++i) {
                WkyRef head,moved=root;
                if (!__wky_mem_alloc(&head,1,sizeof(Node))) abort();
                Node *p=node(head);
                *(uint64_t *)__wky_mem_write_address(&head,&p->value,8)=i*17+seed;
                p=node(head);
                root=(WkyRef){0};
                __wky_mem_store_owner(&p->child,&moved,&head);
                moved=head; head=(WkyRef){0};
                __wky_mem_replace(&root,&moved);
                __wky_mem_drop(&head);
            }
            if (!__wky_mem_clone(&copy,&root)) abort();
            if (size) {
                Node *p=node(copy);
                uint64_t value=p->value+1;
                *(uint64_t *)__wky_mem_write_address(&copy,&p->value,8)=value;
            }
            originals+=chain_sum(root); copies+=chain_sum(copy);
        } else {
            if (!__wky_mem_alloc(&root,size,sizeof(WkyRef))) abort();
            for (uint64_t i=0; i<size; ++i) {
                WkyRef *slot=owner_at(root,i),value;
                if (!__wky_mem_alloc(&value,1,8)) abort();
                __wky_mem_store_owner(slot,&value,&root);
                WkyRef leaf=*owner_at(root,i);
                uint64_t *p=word(leaf);
                *(uint64_t *)__wky_mem_write_address(&leaf,p,8)=i*17+seed;
            }
            if (!__wky_mem_clone(&copy,&root)) abort();
            if (size) {
                WkyRef leaf=*owner_at(copy,0);
                uint64_t *p=word(leaf);
                uint64_t value=*p+1;
                *(uint64_t *)__wky_mem_write_address(&leaf,p,8)=value;
            }
            originals+=fanout_sum(root); copies+=fanout_sum(copy);
        }
        __wky_mem_drop(&copy); __wky_mem_drop(&root);
    }
    printf("%"PRIu64" %"PRIu64" %"PRIu64" %"PRIu64" %"PRIu64" %"PRIu64" %"PRIu64" %"PRIu64" %"PRIu64"\n",
        originals,copies,__wky_mem_metric(0),__wky_mem_metric(1),__wky_mem_metric(2),
        __wky_mem_metric(3),__wky_mem_metric(5),__wky_mem_metric(4),__wky_mem_metric(7));
}
