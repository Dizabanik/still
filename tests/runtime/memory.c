#include "kawa_memory.h"
#include <assert.h>
#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <signal.h>

typedef struct { int64_t value; KawaRef child, other; } Node;
static Node *node(KawaRef r, uint64_t i) {
    return __kawa_mem_address(r.descriptor,r.generation,r.offset,r.extent,i,sizeof(Node));
}
static KawaRef watched[8];
static unsigned watched_count;
static uint64_t watched_live;
static void unchanged_on_abort(int signal_number) {
    signal(SIGABRT,SIG_DFL);
    assert(signal_number==SIGABRT);
    assert(__kawa_mem_metric(KAWA_MEM_LIVE_BYTES)==watched_live);
    for (unsigned i=0; i<watched_count; ++i) {
        assert(__kawa_mem_try_pin(&watched[i]));
        __kawa_mem_unpin(&watched[i]);
    }
}
static void watch(KawaRef root, KawaRef child, KawaRef leaf) {
    watched[0]=root; watched[1]=child; watched[2]=leaf; watched_count=3;
    watched_live=__kawa_mem_metric(KAWA_MEM_LIVE_BYTES);
    signal(SIGABRT,unchanged_on_abort);
}
static int nested(const char *test) {
    if (strncmp(test,"nested",6)) return 0;
    KawaRef root,child,leaf,other,copy,view;
    assert(__kawa_mem_alloc(&root,3,sizeof(Node)));
    assert(__kawa_mem_alloc(&child,2,sizeof(Node)));
    assert(__kawa_mem_alloc(&leaf,2,sizeof(int64_t)));
    assert(__kawa_mem_alloc(&other,1,sizeof(int64_t)));
    KawaRef child_alias=child, leaf_alias=leaf, other_alias=other;
    node(root,0)->value=13;
    node(child,1)->value=29;
    int64_t *data=__kawa_mem_address(leaf.descriptor,leaf.generation,0,leaf.extent,0,8);
    data[0]=71; data[1]=73;
    __kawa_mem_store_owner(&node(child,1)->child,&leaf,&child);
    __kawa_mem_store_owner(&node(root,0)->child,&child,&root);
    __kawa_mem_store_owner(&node(root,2)->child,&other,&root);
    assert(!child.descriptor && !leaf.descriptor && !other.descriptor);
    if (!strcmp(test,"nested_pinned")) {
        __kawa_mem_pin(&leaf_alias);
        watch(root,child_alias,leaf_alias);
        __kawa_mem_drop(&root); return 99;
    }
    if (!strcmp(test,"nested_replace_pinned")) {
        __kawa_mem_pin(&leaf_alias);
        assert(__kawa_mem_alloc(&child,1,8));
        watch(root,child_alias,leaf_alias);
        __kawa_mem_store_owner(&node(root,0)->child,&child,&root); return 99;
    }
    if (!strcmp(test,"nested_parent_pinned")) {
        __kawa_mem_pin(&root);
        watch(root,child_alias,leaf_alias);
        __kawa_mem_take(&copy,&node(root,0)->child,&root); return 99;
    }
    if (!strcmp(test,"nested_resize_pinned")) {
        __kawa_mem_pin(&leaf_alias);
        watch(root,child_alias,leaf_alias);
        __kawa_mem_resize(&root,0,sizeof(Node)); return 99;
    }
    if (!strcmp(test,"nested_cycle")) {
        watch(root,child_alias,leaf_alias);
        __kawa_mem_store_owner(&node(child_alias,0)->child,&root,&child_alias); return 99;
    }
    if (!strcmp(test,"nested_remove")) {
        __kawa_mem_remove(&leaf_alias); return 99;
    }
    if (!strcmp(test,"nested_stale")) {
        __kawa_mem_drop(&root);
        (void)node(child_alias,0); return 99;
    }
    if (!strcmp(test,"nested_write_stale")) {
        Node *slot=node(root,0);
        view=root;
        __kawa_mem_drop(&root);
        (void)__kawa_mem_write_address(&view,slot,sizeof(Node)); return 99;
    }
    assert(!strcmp(test,"nested"));
    const uint64_t budgets[]={0,215,216,223,224,367,368,383};
    uint64_t live=__kawa_mem_metric(KAWA_MEM_LIVE_BYTES);
    assert(live==3*sizeof(Node)+2*sizeof(Node)+24);
    for (unsigned i=0; i<sizeof(budgets)/sizeof(*budgets); ++i) {
        __kawa_mem_budget(budgets[i]);
        assert(!__kawa_mem_clone(&copy,&root) && !copy.descriptor);
        assert(__kawa_mem_metric(KAWA_MEM_LIVE_BYTES)==live);
        assert(node(root,0)->child.descriptor==child_alias.descriptor);
        assert(node(child_alias,1)->child.descriptor==leaf_alias.descriptor);
        assert(data[0]==71 && data[1]==73);
    }
    __kawa_mem_budget(UINT64_MAX);
    assert(__kawa_mem_clone(&copy,&root));
    Node *cloned=node(copy,0);
    assert(cloned->value==13 && cloned->child.descriptor!=child_alias.descriptor);
    Node *cloned_child=node(cloned->child,1);
    assert(cloned_child->value==29 && cloned_child->child.descriptor!=leaf_alias.descriptor);
    int64_t *cloned_leaf=__kawa_mem_address(cloned_child->child.descriptor,
        cloned_child->child.generation,0,16,0,8);
    assert(cloned_leaf[0]==71 && cloned_leaf[1]==73);
    cloned_leaf[0]=101;
    assert(data[0]==71);
    __kawa_mem_drop(&copy);
    assert(__kawa_mem_metric(KAWA_MEM_LIVE_BYTES)==live);
    __kawa_mem_slice(&view,&root,1,3,sizeof(Node));
    assert(__kawa_mem_clone(&copy,&view));
    assert(copy.extent==2*sizeof(Node));
    assert(!node(copy,0)->child.descriptor);
    assert(node(copy,1)->child.descriptor!=other_alias.descriptor);
    __kawa_mem_drop(&copy);
    KawaRef before_resize=root;
    __kawa_mem_budget(0);
    assert(!__kawa_mem_resize(&root,5,sizeof(Node)));
    assert(root.descriptor==before_resize.descriptor && root.generation==before_resize.generation);
    assert(node(root,0)->child.descriptor==child_alias.descriptor);
    __kawa_mem_budget(UINT64_MAX);
    assert(__kawa_mem_resize(&root,5,sizeof(Node)));
    assert(!node(root,4)->child.descriptor && node(root,4)->value==0);
    assert(__kawa_mem_try_pin(&before_resize)==NULL);
    assert(__kawa_mem_clone(&copy,&root));
    assert(node(node(copy,0)->child,1)->value==29);
    __kawa_mem_drop(&copy);
    for (unsigned i=0; i<12; ++i) {
        KawaRef previous=root;
        assert(__kawa_mem_resize(&root,i%2 ? 3 : 4,sizeof(Node)));
        assert(__kawa_mem_try_pin(&previous)==NULL);
        assert(node(root,0)->child.descriptor==child_alias.descriptor);
    }
    __kawa_mem_pin(&leaf_alias); /* A retained child's payload stays pinned/live. */
    assert(__kawa_mem_resize(&root,1,sizeof(Node)));
    assert(data[0]==71 && __kawa_mem_try_pin(&other_alias)==NULL);
    __kawa_mem_unpin(&leaf_alias);
    assert(__kawa_mem_resize(&root,3,sizeof(Node)));
    assert(!node(root,2)->child.descriptor && node(root,2)->value==0);
    __kawa_mem_take(&copy,&node(root,0)->child,&root);
    assert(copy.descriptor==child_alias.descriptor && !node(root,0)->child.descriptor);
    __kawa_mem_drop(&root);
    assert(__kawa_mem_try_pin(&other_alias)==NULL);
    assert(node(copy,1)->value==29);
    __kawa_mem_drop(&copy);
    assert(__kawa_mem_try_pin(&leaf_alias)==NULL);
    assert(__kawa_mem_metric(KAWA_MEM_LIVE_BYTES)==0);
    /* A chain far deeper than the machine stack, including deep clone. */
    assert(__kawa_mem_alloc(&root,1,sizeof(Node)));
    for (unsigned i=1; i<50000; ++i) {
        assert(__kawa_mem_alloc(&copy,1,sizeof(Node)));
        node(copy,0)->value=i;
        __kawa_mem_store_owner(&node(copy,0)->child,&root,&copy);
        root=copy;
    }
    assert(__kawa_mem_clone(&copy,&root));
    view=copy;
    for (unsigned i=50000; i--;) {
        Node *n=node(view,0);
        assert(n->value==(int64_t)i);
        view=n->child;
    }
    assert(!view.descriptor);
    __kawa_mem_drop(&root); __kawa_mem_drop(&copy);
    assert(__kawa_mem_metric(KAWA_MEM_LIVE_BYTES)==0);
    assert(__kawa_mem_metric(KAWA_MEM_ALLOCATIONS)==__kawa_mem_metric(KAWA_MEM_FREES));
    puts("memory runtime: verified");
    return 1;
}

static int64_t *at(KawaRef r, uint64_t i) {
    return __kawa_mem_address(r.descriptor, r.generation, r.offset, r.extent, i, sizeof(int64_t));
}
static void *foreign(void *arg) {
    /* Acquiring on a foreign thread fails without reading mutable payload. */
    assert(__kawa_mem_try_pin(arg) == NULL);
    return NULL;
}
int main(int argc, char **argv) {
    assert(argc == 2);
    const char *test = argv[1];
    if (nested(test)) return 0;
    KawaRef a, b, r, s;
    assert(__kawa_mem_alloc(&a, 4, sizeof(int64_t)));
    r = a;
    *at(r, 0) = 19;
    if (!strcmp(test, "bounds")) { (void)at(r, 4); return 99; }
    if (!strcmp(test, "overflow")) { (void)at(r, UINT64_MAX / 8 + 1); return 99; }
    if (!strcmp(test, "negative")) { (void)at(r, UINT64_MAX); return 99; }
    if (!strcmp(test, "slice")) { __kawa_mem_slice(&s, &r, 2, 1, 8); return 99; }
    if (!strcmp(test, "pinned")) { __kawa_mem_pin(&r); __kawa_mem_drop(&a); return 99; }
    if (!strcmp(test, "resize_pinned")) { __kawa_mem_pin(&r); __kawa_mem_resize(&a,8,8); return 99; }
    if (!strcmp(test, "resize_stale")) { assert(__kawa_mem_resize(&a,8,8)); (void)at(r,0); return 99; }
    if (!strcmp(test, "stale")) { __kawa_mem_drop(&a); (void)at(r, 0); return 99; }
    if (!strcmp(test, "reuse")) {
        __kawa_mem_drop(&a);
        assert(__kawa_mem_alloc(&b, 4, 8));
        assert(r.descriptor == b.descriptor && r.generation != b.generation);
        *at(b, 0) = 99;
        (void)at(r, 0);
        return 99;
    }
    if (!strcmp(test, "arena_pinned")) {
        assert(__kawa_mem_arena(&b));
        assert(__kawa_mem_arena_alloc(&s, &b, 1, 8));
        __kawa_mem_pin(&s);
        __kawa_mem_drop(&b);
        return 99;
    }
    if (!strcmp(test, "region_pinned")) {
        assert(__kawa_mem_arena(&b));
        assert(__kawa_mem_arena_alloc(&s, &b, 1, 8));
        __kawa_mem_pin(&b);
        __kawa_mem_remove(&s);
        return 99;
    }
    assert(!strcmp(test, "valid"));
    assert(__kawa_mem_clone(&b, &r));
    *at(b, 0) = 27;
    assert(*at(r, 0) == 19 && *at(b, 0) == 27);
    assert(__kawa_mem_metric(KAWA_MEM_CLONED_BYTES) == 32);
    __kawa_mem_slice(&s, &r, 4, 4, 8);
    assert(s.extent == 0 && s.offset == 32);
    assert(__kawa_mem_try_pin(&s) != NULL);
    __kawa_mem_unpin(&s);
    pthread_t thread;
    assert(!pthread_create(&thread, NULL, foreign, &r));
    assert(!pthread_join(thread, NULL));
    __kawa_mem_budget(0);
    assert(!__kawa_mem_resize(&a,5,8));
    assert(!__kawa_mem_resize(&a,UINT64_MAX,8));
    assert(a.generation == r.generation && a.extent==32 && *at(r,0)==19);
    __kawa_mem_budget(UINT64_MAX);
    assert(__kawa_mem_resize(&a,5,8));
    assert(a.extent==40 && __kawa_mem_capacity(&a)==64);
    assert(*at(a,0)==19 && *at(a,4)==0 && __kawa_mem_try_pin(&r)==NULL);
    for (int i=0; i<12; ++i) {
        KawaRef old=a;
        assert(__kawa_mem_resize(&a,i%2 ? 1 : 2,8));
        assert(*at(a,0)==19 && __kawa_mem_try_pin(&old)==NULL);
    }
    assert(__kawa_mem_resize(&a,5,8));
    assert(*at(a,1)==0 && *at(a,4)==0);
    assert(__kawa_mem_metric(KAWA_MEM_REALLOCATIONS)==1);
    assert(__kawa_mem_metric(KAWA_MEM_INVALIDATIONS)==14);
    __kawa_mem_drop(&a);
    assert(__kawa_mem_try_pin(&r) == NULL);
    __kawa_mem_drop(&a); /* moved/empty owners make cleanup idempotent */
    __kawa_mem_drop(&b);
    assert(__kawa_mem_metric(KAWA_MEM_LIVE_BYTES) == 0);
    assert(__kawa_mem_metric(KAWA_MEM_ALLOCATIONS) == __kawa_mem_metric(KAWA_MEM_FREES));
    __kawa_mem_budget(7);
    assert(!__kawa_mem_alloc(&a, 1, 8) && !a.descriptor);
    assert(!__kawa_mem_alloc(&a, UINT64_MAX, 8) && !a.descriptor);
    __kawa_mem_budget(UINT64_MAX);
    uint64_t retired = __kawa_mem_metric(KAWA_MEM_RETIRED_SLOTS);
    for (int i = 0; i < 12; ++i) {
        assert(__kawa_mem_alloc(&a, 1, 8));
        assert(__kawa_mem_try_pin(&r) == NULL);
        __kawa_mem_drop(&a);
    }
#if KAWA_GENERATION_MAX == 3
    assert(__kawa_mem_metric(KAWA_MEM_RETIRED_SLOTS) > retired);
#else
    assert(__kawa_mem_metric(KAWA_MEM_RETIRED_SLOTS) == retired);
#endif
    assert(__kawa_mem_arena(&a));
    assert(__kawa_mem_arena_alloc(&b, &a, 2, 8));
    assert(__kawa_mem_arena_alloc(&s, &a, 2, 8));
    r = b;
    __kawa_mem_remove(&b);
    assert(__kawa_mem_try_pin(&r) == NULL);
    assert(__kawa_mem_try_pin(&s));
    __kawa_mem_unpin(&s);
    __kawa_mem_drop(&a);
    assert(__kawa_mem_try_pin(&s) == NULL);
    assert(__kawa_mem_metric(KAWA_MEM_LIVE_BYTES) == 0);
    assert(__kawa_mem_metric(KAWA_MEM_ALLOCATIONS) == __kawa_mem_metric(KAWA_MEM_FREES));
    puts("memory runtime: verified");
}
