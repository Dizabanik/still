/* Same runtime, checked accesses, zero initialization, transfer, deep clone,
 * and lexical drop order as Kawa. All arithmetic is explicitly uint64_t. */
#include "../../src/runtime/kawa_memory.c"
#include <inttypes.h>
#include <errno.h>

typedef struct { uint64_t value; KawaRef child; } Node;
_Static_assert(sizeof(Node)==40 && sizeof(KawaRef)==32,"benchmark ABI");
static uint64_t input(const char *text) {
    char *end;
    errno=0;
    unsigned long long n=strtoull(text,&end,10);
    if (errno || end==text || *end || *text=='-') exit(2);
    return n;
}
static Node *node(KawaRef r) {
    return __kawa_mem_address(r.descriptor,r.generation,r.offset,r.extent,0,sizeof(Node));
}
static KawaRef *owner_at(KawaRef r,uint64_t i) {
    return __kawa_mem_address(r.descriptor,r.generation,r.offset,r.extent,i,sizeof(KawaRef));
}
static uint64_t *word(KawaRef r) {
    return __kawa_mem_address(r.descriptor,r.generation,r.offset,r.extent,0,8);
}
static uint64_t chain_sum(KawaRef current) {
    uint64_t sum=0;
    while (current.descriptor && current.extent/sizeof(Node)) {
        sum+=node(current)->value;
        current=node(current)->child;
    }
    return sum;
}
static uint64_t fanout_sum(KawaRef current) {
    uint64_t sum=0;
    for (uint64_t i=0; i<current.extent/sizeof(KawaRef); ++i)
        sum+=*word(*owner_at(current,i));
    return sum;
}
int main(int argc,char **argv) {
    if (argc!=5) return 2;
    uint64_t size=input(argv[1]),trials=input(argv[2]),seed=input(argv[3]),shape=input(argv[4]);
    if (size>1000000 || trials<1 || trials>100 || seed>65535 || shape>1) return 2;
    uint64_t originals=0,copies=0;
    for (uint64_t trial=0; trial<trials; ++trial) {
        KawaRef root,copy;
        if (!shape) {
            if (!__kawa_mem_alloc(&root,size ? 1 : 0,sizeof(Node))) abort();
            if (size) {
                Node *p=node(root);
                *(uint64_t *)__kawa_mem_write_address(&root,&p->value,8)=seed;
            }
            for (uint64_t i=1; i<size; ++i) {
                KawaRef head,moved=root;
                if (!__kawa_mem_alloc(&head,1,sizeof(Node))) abort();
                Node *p=node(head);
                *(uint64_t *)__kawa_mem_write_address(&head,&p->value,8)=i*17+seed;
                p=node(head);
                root=(KawaRef){0};
                __kawa_mem_store_owner(&p->child,&moved,&head);
                moved=head; head=(KawaRef){0};
                __kawa_mem_replace(&root,&moved);
                __kawa_mem_drop(&head);
            }
            if (!__kawa_mem_clone(&copy,&root)) abort();
            if (size) {
                Node *p=node(copy);
                uint64_t value=node(copy)->value+1;
                *(uint64_t *)__kawa_mem_write_address(&copy,&p->value,8)=value;
            }
            originals+=chain_sum(root); copies+=chain_sum(copy);
        } else {
            if (!__kawa_mem_alloc(&root,size,sizeof(KawaRef))) abort();
            for (uint64_t i=0; i<size; ++i) {
                KawaRef *slot=owner_at(root,i),value;
                if (!__kawa_mem_alloc(&value,1,8)) abort();
                __kawa_mem_store_owner(slot,&value,&root);
                KawaRef leaf=*owner_at(root,i);
                uint64_t *p=word(leaf);
                *(uint64_t *)__kawa_mem_write_address(&leaf,p,8)=i*17+seed;
            }
            if (!__kawa_mem_clone(&copy,&root)) abort();
            if (size) {
                KawaRef leaf=*owner_at(copy,0);
                uint64_t *p=word(leaf);
                uint64_t value=*word(*owner_at(copy,0))+1;
                *(uint64_t *)__kawa_mem_write_address(&leaf,p,8)=value;
            }
            originals+=fanout_sum(root); copies+=fanout_sum(copy);
        }
        __kawa_mem_drop(&copy); __kawa_mem_drop(&root);
    }
    printf("%"PRIu64" %"PRIu64" %"PRIu64" %"PRIu64" %"PRIu64" %"PRIu64" %"PRIu64" %"PRIu64" %"PRIu64"\n",
        originals,copies,__kawa_mem_metric(0),__kawa_mem_metric(1),__kawa_mem_metric(2),
        __kawa_mem_metric(3),__kawa_mem_metric(5),__kawa_mem_metric(4),__kawa_mem_metric(7));
}
