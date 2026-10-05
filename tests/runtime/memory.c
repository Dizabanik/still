#include "wky_memory.h"
#include <assert.h>
#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <signal.h>

typedef struct { int64_t value; WkyRef child, other; } Node;
static Node *node(WkyRef r, uint64_t i) {
    return __wky_mem_address(r.descriptor,r.generation,r.offset,r.extent,i,sizeof(Node));
}
static WkyRef watched[8];
static unsigned watched_count;
static uint64_t watched_live;
static void *watched_value;
static unsigned char watched_bytes[512];
static size_t watched_size;
static void unchanged_on_abort(int signal_number) {
    signal(SIGABRT,SIG_DFL);
    assert(signal_number==SIGABRT);
    assert(__wky_mem_metric(WKY_MEM_LIVE_BYTES)==watched_live);
    if (watched_value) assert(!memcmp(watched_value,watched_bytes,watched_size));
    for (unsigned i=0; i<watched_count; ++i) {
        assert(__wky_mem_try_pin(&watched[i]));
        __wky_mem_unpin(&watched[i]);
    }
}
static void watch(WkyRef root, WkyRef child, WkyRef leaf) {
    watched[0]=root; watched[1]=child; watched[2]=leaf; watched_count=3;
    watched_live=__wky_mem_metric(WKY_MEM_LIVE_BYTES);
    signal(SIGABRT,unchanged_on_abort);
}
typedef struct { int64_t tag; WkyRef owned[3], borrowed; } Value;
typedef struct { Value item, more[2]; } Pack;
static const WkyOwnedField value_fields[]={{offsetof(Value,owned),3,sizeof(WkyRef),NULL}};
static const WkyValueLayout value_layout={sizeof(Value),1,value_fields};
static const WkyOwnedField pack_fields[]={
    {offsetof(Pack,item),1,sizeof(Value),&value_layout},
    {offsetof(Pack,more),2,sizeof(Value),&value_layout}
};
static const WkyValueLayout pack_layout={sizeof(Pack),2,pack_fields};
static int values(const char *test) {
    if (strncmp(test,"value",5)) return 0;
    Value source={.tag=17}, copy={0}, incoming={0};
    for (unsigned i=0; i<3; ++i) {
        assert(__wky_mem_alloc(&source.owned[i],1,8));
        *(int64_t *)__wky_mem_address(source.owned[i].descriptor,source.owned[i].generation,0,8,0,8)=31+i;
    }
    source.borrowed=source.owned[0];
    if (!strcmp(test,"value_drop_pinned") || !strcmp(test,"value_store_pinned")) {
        watch(source.owned[0],source.owned[1],source.owned[2]);
        watched_value=&source; watched_size=sizeof(source); memcpy(watched_bytes,&source,sizeof(source));
        __wky_mem_pin(&source.owned[2]);
        if (!strcmp(test,"value_drop_pinned")) __wky_mem_value_drop(&source,&value_layout);
        else __wky_mem_value_store(&source,&incoming,&value_layout,NULL);
        return 99;
    }
    if (!strcmp(test,"value_duplicate")) {
        incoming.owned[0]=source.owned[0]; incoming.owned[1]=source.owned[0];
        __wky_mem_value_store(&copy,&incoming,&value_layout,NULL); return 99;
    }
    Value original=source;
    for (uint64_t budget=0; budget<24; ++budget) {
        __wky_mem_budget(budget);
        assert(!__wky_mem_value_clone(&copy,&source,&value_layout,NULL));
        assert(!memcmp(&source,&original,sizeof(source)));
        Value zero={0}; assert(!memcmp(&copy,&zero,sizeof(copy)));
        assert(__wky_mem_metric(WKY_MEM_LIVE_BYTES)==24);
    }
    __wky_mem_budget(UINT64_MAX);
    assert(__wky_mem_value_clone(&copy,&source,&value_layout,NULL));
    assert(copy.tag==17 && copy.borrowed.descriptor==source.borrowed.descriptor);
    for (unsigned i=0; i<3; ++i) {
        assert(copy.owned[i].descriptor!=source.owned[i].descriptor);
        assert(*(int64_t *)__wky_mem_address(copy.owned[i].descriptor,copy.owned[i].generation,0,8,0,8)==(int64_t)(31+i));
    }
    __wky_mem_value_drop(&copy,&value_layout);
    Pack pack={0};
    __wky_mem_value_take(&pack.more[1],&source,&value_layout,NULL);
    assert(!source.owned[0].descriptor && pack.more[1].tag==17);
    assert(__wky_mem_value_clone(&pack.item,&pack.more[1],&value_layout,NULL));
    WkyRef heap;
    assert(__wky_mem_alloc(&heap,1,sizeof(Pack)));
    Pack *destination=__wky_mem_address(heap.descriptor,heap.generation,0,heap.extent,0,sizeof(Pack));
    __wky_mem_value_store(destination,&pack,&pack_layout,&heap);
    assert(!pack.item.owned[0].descriptor && !pack.more[1].owned[0].descriptor);
    if (!strcmp(test,"value_take_pinned")) {
        watch(heap,destination->item.owned[0],destination->more[1].owned[0]);
        __wky_mem_pin(&heap);
        __wky_mem_value_take(&pack,destination,&pack_layout,&heap); return 99;
    }
    if (!strcmp(test,"value_stale")) {
        WkyRef alias=heap;
        __wky_mem_drop(&heap);
        __wky_mem_value_take(&pack,destination,&pack_layout,&alias); return 99;
    }
    assert(!strcmp(test,"value"));
    assert(__wky_mem_value_clone(&copy,&destination->item,&value_layout,&heap));
    __wky_mem_value_drop(&copy,&value_layout);
    __wky_mem_value_take(&pack,destination,&pack_layout,&heap);
    __wky_mem_drop(&heap);
    assert(__wky_mem_try_pin(&pack.more[1].borrowed));
    __wky_mem_unpin(&pack.more[1].borrowed);
    __wky_mem_value_drop(&pack,&pack_layout);
    assert(__wky_mem_metric(WKY_MEM_LIVE_BYTES)==0);
    puts("memory runtime: verified");
    return 1;
}
static int nested(const char *test) {
    if (strncmp(test,"nested",6)) return 0;
    WkyRef root,child,leaf,other,copy,view;
    assert(__wky_mem_alloc(&root,3,sizeof(Node)));
    assert(__wky_mem_alloc(&child,2,sizeof(Node)));
    assert(__wky_mem_alloc(&leaf,2,sizeof(int64_t)));
    assert(__wky_mem_alloc(&other,1,sizeof(int64_t)));
    WkyRef child_alias=child, leaf_alias=leaf, other_alias=other;
    node(root,0)->value=13;
    node(child,1)->value=29;
    int64_t *data=__wky_mem_address(leaf.descriptor,leaf.generation,0,leaf.extent,0,8);
    data[0]=71; data[1]=73;
    __wky_mem_store_owner(&node(child,1)->child,&leaf,&child);
    __wky_mem_store_owner(&node(root,0)->child,&child,&root);
    __wky_mem_store_owner(&node(root,2)->child,&other,&root);
    assert(!child.descriptor && !leaf.descriptor && !other.descriptor);
    if (!strcmp(test,"nested_pinned")) {
        __wky_mem_pin(&leaf_alias);
        watch(root,child_alias,leaf_alias);
        __wky_mem_drop(&root); return 99;
    }
    if (!strcmp(test,"nested_replace_pinned")) {
        __wky_mem_pin(&leaf_alias);
        assert(__wky_mem_alloc(&child,1,8));
        watch(root,child_alias,leaf_alias);
        __wky_mem_store_owner(&node(root,0)->child,&child,&root); return 99;
    }
    if (!strcmp(test,"nested_parent_pinned")) {
        __wky_mem_pin(&root);
        watch(root,child_alias,leaf_alias);
        __wky_mem_take(&copy,&node(root,0)->child,&root); return 99;
    }
    if (!strcmp(test,"nested_resize_pinned")) {
        __wky_mem_pin(&leaf_alias);
        watch(root,child_alias,leaf_alias);
        __wky_mem_resize(&root,0,sizeof(Node)); return 99;
    }
    if (!strcmp(test,"nested_cycle")) {
        watch(root,child_alias,leaf_alias);
        __wky_mem_store_owner(&node(child_alias,0)->child,&root,&child_alias); return 99;
    }
    if (!strcmp(test,"nested_remove")) {
        __wky_mem_remove(&leaf_alias); return 99;
    }
    if (!strcmp(test,"nested_stale")) {
        __wky_mem_drop(&root);
        (void)node(child_alias,0); return 99;
    }
    if (!strcmp(test,"nested_write_stale")) {
        Node *slot=node(root,0);
        view=root;
        __wky_mem_drop(&root);
        (void)__wky_mem_write_address(&view,slot,sizeof(Node)); return 99;
    }
    assert(!strcmp(test,"nested"));
    const uint64_t budgets[]={0,215,216,223,224,367,368,383};
    uint64_t live=__wky_mem_metric(WKY_MEM_LIVE_BYTES);
    assert(live==3*sizeof(Node)+2*sizeof(Node)+24);
    for (unsigned i=0; i<sizeof(budgets)/sizeof(*budgets); ++i) {
        __wky_mem_budget(budgets[i]);
        assert(!__wky_mem_clone(&copy,&root) && !copy.descriptor);
        assert(__wky_mem_metric(WKY_MEM_LIVE_BYTES)==live);
        assert(node(root,0)->child.descriptor==child_alias.descriptor);
        assert(node(child_alias,1)->child.descriptor==leaf_alias.descriptor);
        assert(data[0]==71 && data[1]==73);
    }
    __wky_mem_budget(UINT64_MAX);
    assert(__wky_mem_clone(&copy,&root));
    Node *cloned=node(copy,0);
    assert(cloned->value==13 && cloned->child.descriptor!=child_alias.descriptor);
    Node *cloned_child=node(cloned->child,1);
    assert(cloned_child->value==29 && cloned_child->child.descriptor!=leaf_alias.descriptor);
    int64_t *cloned_leaf=__wky_mem_address(cloned_child->child.descriptor,
        cloned_child->child.generation,0,16,0,8);
    assert(cloned_leaf[0]==71 && cloned_leaf[1]==73);
    cloned_leaf[0]=101;
    assert(data[0]==71);
    __wky_mem_drop(&copy);
    assert(__wky_mem_metric(WKY_MEM_LIVE_BYTES)==live);
    __wky_mem_slice(&view,&root,1,3,sizeof(Node));
    assert(__wky_mem_clone(&copy,&view));
    assert(copy.extent==2*sizeof(Node));
    assert(!node(copy,0)->child.descriptor);
    assert(node(copy,1)->child.descriptor!=other_alias.descriptor);
    __wky_mem_drop(&copy);
    WkyRef before_resize=root;
    __wky_mem_budget(0);
    assert(!__wky_mem_resize(&root,5,sizeof(Node)));
    assert(root.descriptor==before_resize.descriptor && root.generation==before_resize.generation);
    assert(node(root,0)->child.descriptor==child_alias.descriptor);
    __wky_mem_budget(UINT64_MAX);
    assert(__wky_mem_resize(&root,5,sizeof(Node)));
    assert(!node(root,4)->child.descriptor && node(root,4)->value==0);
    assert(__wky_mem_try_pin(&before_resize)==NULL);
    assert(__wky_mem_clone(&copy,&root));
    assert(node(node(copy,0)->child,1)->value==29);
    __wky_mem_drop(&copy);
    for (unsigned i=0; i<12; ++i) {
        WkyRef previous=root;
        assert(__wky_mem_resize(&root,i%2 ? 3 : 4,sizeof(Node)));
        assert(__wky_mem_try_pin(&previous)==NULL);
        assert(node(root,0)->child.descriptor==child_alias.descriptor);
    }
    __wky_mem_pin(&leaf_alias); /* A retained child's payload stays pinned/live. */
    assert(__wky_mem_resize(&root,1,sizeof(Node)));
    assert(data[0]==71 && __wky_mem_try_pin(&other_alias)==NULL);
    __wky_mem_unpin(&leaf_alias);
    assert(__wky_mem_resize(&root,3,sizeof(Node)));
    assert(!node(root,2)->child.descriptor && node(root,2)->value==0);
    __wky_mem_take(&copy,&node(root,0)->child,&root);
    assert(copy.descriptor==child_alias.descriptor && !node(root,0)->child.descriptor);
    __wky_mem_drop(&root);
    assert(__wky_mem_try_pin(&other_alias)==NULL);
    assert(node(copy,1)->value==29);
    __wky_mem_drop(&copy);
    assert(__wky_mem_try_pin(&leaf_alias)==NULL);
    assert(__wky_mem_metric(WKY_MEM_LIVE_BYTES)==0);
    /* A chain far deeper than the machine stack, including deep clone. */
    assert(__wky_mem_alloc(&root,1,sizeof(Node)));
    for (unsigned i=1; i<50000; ++i) {
        assert(__wky_mem_alloc(&copy,1,sizeof(Node)));
        node(copy,0)->value=i;
        __wky_mem_store_owner(&node(copy,0)->child,&root,&copy);
        root=copy;
    }
    assert(__wky_mem_clone(&copy,&root));
    view=copy;
    for (unsigned i=50000; i--;) {
        Node *n=node(view,0);
        assert(n->value==(int64_t)i);
        view=n->child;
    }
    assert(!view.descriptor);
    __wky_mem_drop(&root); __wky_mem_drop(&copy);
    assert(__wky_mem_metric(WKY_MEM_LIVE_BYTES)==0);
    assert(__wky_mem_metric(WKY_MEM_ALLOCATIONS)==__wky_mem_metric(WKY_MEM_FREES));
    puts("memory runtime: verified");
    return 1;
}

static int64_t *at(WkyRef r, uint64_t i) {
    return __wky_mem_address(r.descriptor, r.generation, r.offset, r.extent, i, sizeof(int64_t));
}
static void *foreign(void *arg) {
    /* Acquiring on a foreign thread fails without reading mutable payload. */
    assert(__wky_mem_try_pin(arg) == NULL);
    return NULL;
}
int main(int argc, char **argv) {
    assert(argc == 2);
    const char *test = argv[1];
    if (nested(test)) return 0;
    if (values(test)) return 0;
    WkyRef a, b, r, s;
    assert(__wky_mem_alloc(&a, 4, sizeof(int64_t)));
    r = a;
    *at(r, 0) = 19;
    if (!strcmp(test, "bounds")) { (void)at(r, 4); return 99; }
    if (!strcmp(test, "overflow")) { (void)at(r, UINT64_MAX / 8 + 1); return 99; }
    if (!strcmp(test, "negative")) { (void)at(r, UINT64_MAX); return 99; }
    if (!strcmp(test, "slice")) { __wky_mem_slice(&s, &r, 2, 1, 8); return 99; }
    if (!strcmp(test, "view_bounds")) {
        __wky_mem_view(&s,&r,at(r,1),8);
        (void)at(s,1); return 99;
    }
    if (!strcmp(test, "view_outside")) {
        __wky_mem_slice(&s,&r,1,2,8);
        __wky_mem_view(&b,&s,at(r,0),8); return 99;
    }
    if (!strcmp(test, "view_stale")) {
        void *slot=at(r,0);
        __wky_mem_drop(&a);
        __wky_mem_view(&s,&r,slot,8); return 99;
    }
    if (!strcmp(test, "pinned")) { __wky_mem_pin(&r); __wky_mem_drop(&a); return 99; }
    if (!strcmp(test, "resize_pinned")) { __wky_mem_pin(&r); __wky_mem_resize(&a,8,8); return 99; }
    if (!strcmp(test, "resize_stale")) { assert(__wky_mem_resize(&a,8,8)); (void)at(r,0); return 99; }
    if (!strcmp(test, "stale")) { __wky_mem_drop(&a); (void)at(r, 0); return 99; }
    if (!strcmp(test, "reuse")) {
        __wky_mem_drop(&a);
        assert(__wky_mem_alloc(&b, 4, 8));
        assert(r.descriptor == b.descriptor && r.generation != b.generation);
        *at(b, 0) = 99;
        (void)at(r, 0);
        return 99;
    }
    if (!strcmp(test, "arena_pinned")) {
        assert(__wky_mem_arena(&b));
        assert(__wky_mem_arena_alloc(&s, &b, 1, 8));
        __wky_mem_pin(&s);
        __wky_mem_drop(&b);
        return 99;
    }
    if (!strcmp(test, "region_pinned")) {
        assert(__wky_mem_arena(&b));
        assert(__wky_mem_arena_alloc(&s, &b, 1, 8));
        __wky_mem_pin(&b);
        __wky_mem_remove(&s);
        return 99;
    }
    assert(!strcmp(test, "valid"));
    __wky_mem_view(&s,&r,at(r,0),8);
    assert(s.descriptor==r.descriptor && s.generation==r.generation && s.extent==8);
    *at(s,0)=23;
    assert(*at(r,0)==23);
    __wky_mem_view(&s,&r,at(r,3)+1,0);
    assert(s.offset==32 && s.extent==0);
    assert(__wky_mem_try_pin(&s));
    __wky_mem_unpin(&s);
    __wky_mem_view(&s,&r,at(r,1),16);
    __wky_mem_view(&s,&s,at(s,1),8); /* Output may alias the container header. */
    assert(s.offset==16 && s.extent==8);
    assert(__wky_mem_metric(WKY_MEM_VIEWS)==4);
    *at(r,0)=19;
    assert(__wky_mem_clone(&b, &r));
    *at(b, 0) = 27;
    assert(*at(r, 0) == 19 && *at(b, 0) == 27);
    assert(__wky_mem_metric(WKY_MEM_CLONED_BYTES) == 32);
    __wky_mem_slice(&s, &r, 4, 4, 8);
    assert(s.extent == 0 && s.offset == 32);
    assert(__wky_mem_try_pin(&s) != NULL);
    __wky_mem_unpin(&s);
    pthread_t thread;
    assert(!pthread_create(&thread, NULL, foreign, &r));
    assert(!pthread_join(thread, NULL));
    __wky_mem_budget(0);
    assert(!__wky_mem_resize(&a,5,8));
    assert(!__wky_mem_resize(&a,UINT64_MAX,8));
    assert(a.generation == r.generation && a.extent==32 && *at(r,0)==19);
    __wky_mem_budget(UINT64_MAX);
    assert(__wky_mem_resize(&a,5,8));
    assert(a.extent==40 && __wky_mem_capacity(&a)==64);
    assert(*at(a,0)==19 && *at(a,4)==0 && __wky_mem_try_pin(&r)==NULL);
    for (int i=0; i<12; ++i) {
        WkyRef old=a;
        assert(__wky_mem_resize(&a,i%2 ? 1 : 2,8));
        assert(*at(a,0)==19 && __wky_mem_try_pin(&old)==NULL);
    }
    assert(__wky_mem_resize(&a,5,8));
    assert(*at(a,1)==0 && *at(a,4)==0);
    assert(__wky_mem_metric(WKY_MEM_REALLOCATIONS)==1);
    assert(__wky_mem_metric(WKY_MEM_INVALIDATIONS)==14);
    __wky_mem_drop(&a);
    assert(__wky_mem_try_pin(&r) == NULL);
    __wky_mem_drop(&a); /* moved/empty owners make cleanup idempotent */
    __wky_mem_drop(&b);
    assert(__wky_mem_metric(WKY_MEM_LIVE_BYTES) == 0);
    assert(__wky_mem_metric(WKY_MEM_ALLOCATIONS) == __wky_mem_metric(WKY_MEM_FREES));
    __wky_mem_budget(7);
    assert(!__wky_mem_alloc(&a, 1, 8) && !a.descriptor);
    assert(!__wky_mem_alloc(&a, UINT64_MAX, 8) && !a.descriptor);
    __wky_mem_budget(UINT64_MAX);
    uint64_t retired = __wky_mem_metric(WKY_MEM_RETIRED_SLOTS);
    for (int i = 0; i < 12; ++i) {
        assert(__wky_mem_alloc(&a, 1, 8));
        assert(__wky_mem_try_pin(&r) == NULL);
        __wky_mem_drop(&a);
    }
#if WKY_GENERATION_MAX == 3
    assert(__wky_mem_metric(WKY_MEM_RETIRED_SLOTS) > retired);
#else
    assert(__wky_mem_metric(WKY_MEM_RETIRED_SLOTS) == retired);
#endif
    assert(__wky_mem_arena(&a));
    assert(__wky_mem_arena_alloc(&b, &a, 2, 8));
    assert(__wky_mem_arena_alloc(&s, &a, 2, 8));
    r = b;
    __wky_mem_remove(&b);
    assert(__wky_mem_try_pin(&r) == NULL);
    assert(__wky_mem_try_pin(&s));
    __wky_mem_unpin(&s);
    __wky_mem_drop(&a);
    assert(__wky_mem_try_pin(&s) == NULL);
    assert(__wky_mem_metric(WKY_MEM_LIVE_BYTES) == 0);
    assert(__wky_mem_metric(WKY_MEM_ALLOCATIONS) == __wky_mem_metric(WKY_MEM_FREES));
    puts("memory runtime: verified");
}
