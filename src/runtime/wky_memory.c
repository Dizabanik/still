#include "wky_memory.h"
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <pthread.h>

/* Metadata pages are process-lived and reused. A stale reference may always
 * examine its descriptor, even after its arena and allocation are destroyed.
 * Retiring exhausted slots avoids ABA; the small counter test build exercises
 * the otherwise impractical exhaustion path. */
#ifndef WKY_GENERATION_MAX
#define WKY_GENERATION_MAX UINT64_MAX
#endif
#define PAGE_SLOTS 256
struct WkyDescriptor {
    _Atomic uint64_t thread;
    _Atomic uint64_t generation;
    void *data;
    uint64_t bytes, capacity, pins;
    unsigned arena;
    WkyRef *owner_slot;
    WkyDescriptor *parent, *first, *last, *previous, *next, *free_next;
};
typedef struct Page { struct Page *next; WkyDescriptor slots[PAGE_SLOTS]; } Page;
static Page *pages;
static WkyDescriptor *free_slots;
static pthread_mutex_t pool_lock = PTHREAD_MUTEX_INITIALIZER;
static _Atomic uint64_t next_thread = 1;
static _Atomic uint64_t descriptor_bytes, retired_slots;
static _Thread_local uint64_t thread_id;
static _Thread_local uint64_t budget = UINT64_MAX;
static _Thread_local uint64_t allocations, frees, live_bytes, peak_bytes, cloned_bytes;
static _Thread_local uint64_t reallocations, invalidations;
#ifdef WKY_MEMORY_METRICS
static _Thread_local uint64_t checks, pins, views;
#define COUNT(x) (++(x))
#else
#define COUNT(x) ((void)0)
#endif

_Noreturn static void fail(const char *message) {
    fprintf(stderr, "Whisky memory trap: %s\n", message);
    abort();
}
__attribute__((noinline,cold)) static uint64_t initialize_thread(void) {
        /* Saturate instead of allowing thread identities to wrap. */
        uint64_t id = atomic_load_explicit(&next_thread, memory_order_relaxed);
        do {
            if (id == UINT64_MAX) fail("thread identity exhausted");
        } while (!atomic_compare_exchange_weak_explicit(&next_thread, &id, id + 1,
                         memory_order_relaxed, memory_order_relaxed));
        thread_id = id;
    return thread_id;
}
static uint64_t current_thread(void) {
    return thread_id ? thread_id : initialize_thread();
}
static WkyDescriptor *slot_new(void) {
    pthread_mutex_lock(&pool_lock);
    if (!free_slots) {
        Page *page = calloc(1, sizeof(*page));
        if (!page) { pthread_mutex_unlock(&pool_lock); return NULL; }
        page->next = pages;
        pages = page;
        atomic_fetch_add_explicit(&descriptor_bytes, sizeof(*page), memory_order_relaxed);
        for (unsigned i = 0; i < PAGE_SLOTS; ++i) {
            WkyDescriptor *d = &page->slots[i];
            atomic_init(&d->thread, 0);
            atomic_init(&d->generation, 1);
            d->free_next = free_slots;
            free_slots = d;
        }
    }
    WkyDescriptor *d = free_slots;
    free_slots = d->free_next;
    pthread_mutex_unlock(&pool_lock);
    return d;
}
static void slot_release(WkyDescriptor *d) {
    uint64_t generation = atomic_load_explicit(&d->generation, memory_order_relaxed);
    atomic_store_explicit(&d->thread, 0, memory_order_release);
    if (generation == WKY_GENERATION_MAX) {
        atomic_fetch_add_explicit(&retired_slots, 1, memory_order_relaxed);
        return;
    }
    atomic_store_explicit(&d->generation, generation + 1, memory_order_release);
    pthread_mutex_lock(&pool_lock);
    d->free_next = free_slots;
    free_slots = d;
    pthread_mutex_unlock(&pool_lock);
}
/* Check thread ownership before touching non-atomic payload fields. A foreign
 * thread can reject a reference without racing an owner freeing its payload. */
static int alive(const WkyRef *r) {
    WkyDescriptor *d = r->descriptor;
    return d && atomic_load_explicit(&d->thread, memory_order_acquire) == current_thread()
        && atomic_load_explicit(&d->generation, memory_order_acquire) == r->generation;
}
static WkyDescriptor *validate(const WkyRef *r) {
    if (!alive(r)) fail("stale reference or foreign thread");
    WkyDescriptor *d = r->descriptor;
    if (r->offset > d->bytes || r->extent > d->bytes - r->offset)
        fail("invalid reference extent");
    return d;
}
static int allocate(WkyRef *out, uint64_t count, uint64_t size, unsigned arena) {
    *out = (WkyRef){0};
    if (!size || count > SIZE_MAX / size) return 0;
    uint64_t bytes = count * size;
    if (bytes > budget || bytes > UINT64_MAX - live_bytes) return 0;
    /* Even an empty allocation has a unique identity and a non-null address. */
    void *data = calloc(1, bytes ? (size_t)bytes : 1);
    if (!data) return 0;
    WkyDescriptor *d = slot_new();
    if (!d) { free(data); return 0; }
    d->data = data;
    d->bytes = bytes;
    d->capacity = bytes;
    d->pins = 0;
    d->arena = arena;
    d->owner_slot = NULL;
    d->parent = d->first = d->last = d->previous = d->next = NULL;
    atomic_store_explicit(&d->thread, current_thread(), memory_order_release);
    *out = (WkyRef){ d, atomic_load_explicit(&d->generation, memory_order_relaxed), 0, bytes };
    if (budget != UINT64_MAX) budget -= bytes;
    ++allocations;
    live_bytes += bytes;
    if (live_bytes > peak_bytes) peak_bytes = live_bytes;
    return 1;
}
int32_t __wky_mem_alloc(WkyRef *out, uint64_t count, uint64_t size) {
    return allocate(out, count, size, 0);
}
static void detach(WkyDescriptor *d) {
    if (d->previous) d->previous->next = d->next;
    else if (d->parent) d->parent->first = d->next;
    if (d->next) d->next->previous = d->previous;
    else if (d->parent) d->parent->last = d->previous;
    d->parent = d->previous = d->next = NULL;
    d->owner_slot = NULL;
}
static void attach(WkyDescriptor *d, WkyDescriptor *parent, WkyRef *slot, int last) {
    d->parent = parent;
    d->owner_slot = slot;
    if (last) {
        d->previous = parent->last;
        if (d->previous) d->previous->next = d;
        else parent->first = d;
        parent->last = d;
    } else {
        d->next = parent->first;
        if (d->next) d->next->previous = d;
        else parent->last = d;
        parent->first = d;
    }
}
static void dispose(WkyDescriptor *d) {
    if (d->owner_slot) *d->owner_slot = (WkyRef){0};
    detach(d);
    free(d->data);
    live_bytes -= d->capacity;
    ++frees;
    d->data = NULL;
    slot_release(d);
}
static void ancestors_unpinned(WkyDescriptor *d) {
    for (; d; d = d->parent)
        if (d->pins) fail(d->arena ? "arena invalidation during stable access" :
                                     "invalidation during stable access");
}
/* The ownership forest doubles as drop/clone metadata. Plain allocations
 * have no children and retain their fast path. Traversal needs no recursive
 * C stack, per-element bitmap, or reference counting. */
static void preflight(WkyDescriptor *root) {
    WkyDescriptor *d = root;
    for (;;) {
        if (d->pins) fail(root->arena ? "arena invalidation during stable access" :
                                        "invalidation during stable access");
        if (d->first) { d = d->first; continue; }
        while (d != root && !d->next) d = d->parent;
        if (d == root) return;
        d = d->next;
    }
}
static void dispose_tree(WkyDescriptor *root) {
    WkyDescriptor *d = root;
    for (;;) {
        if (d->first) { d = d->first; continue; }
        WkyDescriptor *parent = d->parent;
        int done = d == root;
        dispose(d);
        if (done) return;
        d = parent;
    }
}
static WkyDescriptor *whole_owner(const WkyRef *r) {
    WkyDescriptor *d = validate(r);
    if (r->offset || r->extent != d->bytes)
        fail("operation requires a whole owner");
    return d;
}
void *__wky_mem_write_address(const WkyRef *container, void *slot, uint64_t size) {
    WkyDescriptor *d = validate(container);
    uintptr_t base = (uintptr_t)d->data, address = (uintptr_t)slot;
    if (d->arena || address < base) fail("invalid managed slot");
    uint64_t offset = address - base;
    if (offset < container->offset || offset - container->offset > container->extent ||
        size > container->extent - (offset - container->offset))
        fail("managed slot outside reference bounds");
    return slot;
}
static WkyDescriptor *owning_slot(WkyRef *slot, const WkyRef *container) {
    __wky_mem_write_address(container, slot, sizeof(*slot));
    if ((uintptr_t)slot % _Alignof(WkyRef)) fail("invalid owning slot");
    return container->descriptor;
}
void __wky_mem_view(WkyRef *out,const WkyRef *container,void *slot,uint64_t size) {
    COUNT(views);
    __wky_mem_write_address(container,slot,size);
    *out=*container;
    out->offset=(uintptr_t)slot-(uintptr_t)container->descriptor->data;
    out->extent=size;
}
void __wky_mem_drop(WkyRef *r) {
    if (!r->descriptor) return; /* moved-from/failed owner */
    WkyDescriptor *d = whole_owner(r);
    ancestors_unpinned(d->parent);
    preflight(d); /* Check the entire forest before changing any state. */
    dispose_tree(d);
    *r = (WkyRef){0};
}
void __wky_mem_take(WkyRef *out, WkyRef *slot, const WkyRef *container) {
    WkyDescriptor *parent = owning_slot(slot, container);
    ancestors_unpinned(parent);
    WkyRef value = *slot;
    if (value.descriptor) {
        WkyDescriptor *d = whole_owner(&value);
        if (d->owner_slot != slot || d->parent != parent) fail("unregistered owning slot");
        detach(d);
    }
    *slot = (WkyRef){0};
    *out = value;
}
void __wky_mem_replace(WkyRef *slot, WkyRef *incoming) {
    if (incoming->descriptor) {
        WkyDescriptor *d=whole_owner(incoming);
        if (d->parent) fail("owner must be moved before transfer");
        if (incoming->descriptor==slot->descriptor) fail("owner cannot be duplicated");
    }
    if (slot->descriptor && whole_owner(slot)->parent) fail("replacement requires a root owner");
    __wky_mem_drop(slot);
    *slot=*incoming;
    *incoming=(WkyRef){0};
}
void __wky_mem_store_owner(WkyRef *slot, WkyRef *incoming, const WkyRef *container) {
    WkyDescriptor *parent = owning_slot(slot, container), *next = NULL, *old = NULL;
    ancestors_unpinned(parent);
    if (incoming->descriptor) {
        next = whole_owner(incoming);
        if (next->parent) fail("owner must be moved before transfer");
        if (next->arena) fail("arenas cannot be embedded in owning slots");
        for (WkyDescriptor *d = parent; d; d = d->parent)
            if (d == next) fail("ownership cycle");
    }
    if (slot->descriptor) {
        old = whole_owner(slot);
        if (old->owner_slot != slot || old->parent != parent) fail("unregistered owning slot");
        preflight(old);
    }
    if (old) dispose_tree(old);
    *slot = *incoming;
    *incoming = (WkyRef){0};
    if (next) attach(next, parent, slot, 0);
}
static int child_in_view(WkyDescriptor *d, uint64_t offset, uint64_t extent) {
    if (!d->owner_slot) fail("cannot clone arena ownership");
    uint64_t position = (uintptr_t)d->owner_slot - (uintptr_t)d->parent->data;
    if (position >= offset && position - offset < extent) {
        if (sizeof(WkyRef) > extent - (position - offset)) fail("partial owning slot in clone");
        return 1;
    }
    if (position < offset && offset - position < sizeof(WkyRef))
        fail("partial owning slot in clone");
    return 0;
}
static WkyDescriptor *child_next(WkyDescriptor *d, uint64_t offset, uint64_t extent) {
    while (d && !child_in_view(d, offset, extent)) d = d->next;
    return d;
}
static int copy_allocation(WkyRef *out, WkyDescriptor *source, uint64_t offset, uint64_t extent) {
    if (source->arena) fail("cannot clone an arena");
    if (!allocate(out, extent, 1, 0)) return 0;
    memcpy(out->descriptor->data, (unsigned char *)source->data + offset, extent);
    cloned_bytes += extent;
    /* Clear every copied owning header before any child allocation can fail.
     * Failure cleanup must never acquire ownership of source allocations. */
    for (WkyDescriptor *d = source->first; d; d = d->next) {
        if (!child_in_view(d, offset, extent)) continue;
        uint64_t position = (uintptr_t)d->owner_slot - (uintptr_t)source->data - offset;
        *(WkyRef *)((unsigned char *)out->descriptor->data + position) = (WkyRef){0};
    }
    return 1;
}
int32_t __wky_mem_clone(WkyRef *out, const WkyRef *r) {
    WkyDescriptor *root = validate(r);
    if (!copy_allocation(out, root, r->offset, r->extent)) return 0;
    WkyDescriptor *source = root, *dest = out->descriptor;
    WkyDescriptor *child = child_next(root->first, r->offset, r->extent);
    while (child) {
        WkyRef copy;
        if (!copy_allocation(&copy, child, 0, child->bytes)) {
            dispose_tree(out->descriptor);
            *out = (WkyRef){0};
            return 0;
        }
        uint64_t position = (uintptr_t)child->owner_slot - (uintptr_t)source->data -
                            (source == root ? r->offset : 0);
        WkyRef *slot = (WkyRef *)((unsigned char *)dest->data + position);
        *slot = copy;
        attach(copy.descriptor, dest, slot, 1); /* Preserve acquisition/drop order. */
        if (child->first) {
            source = child; dest = copy.descriptor; child = child->first;
            continue;
        }
        for (;;) {
            WkyDescriptor *next = child_next(child->next,
                source == root ? r->offset : 0, source == root ? r->extent : source->bytes);
            if (next) { child = next; break; }
            if (source == root) return 1;
            child = source; source = source->parent; dest = dest->parent;
        }
    }
    return 1;
}
/* Layout depth is bounded by finite source types. Allocation trees themselves
 * still use the iterative drop/clone paths above. Aggregate operations need
 * no descriptor, dynamic offset table, or per-field reference count. */
typedef int (*ValueVisit)(WkyRef *, void *);
static int value_walk(void *slot, const WkyValueLayout *layout, int reverse,
                      ValueVisit visit, void *context) {
    for (uint64_t at=0; at<layout->count; ++at) {
        uint64_t i=reverse ? layout->count-1-at : at;
        const WkyOwnedField *field=&layout->fields[i];
        for (uint64_t element=0; element<field->count; ++element) {
            uint64_t j=reverse ? field->count-1-element : element;
            void *child=(unsigned char *)slot+field->offset+j*field->stride;
            if (field->layout) {
                if (!value_walk(child,field->layout,reverse,visit,context)) return 0;
            } else if (!visit(child,context)) return 0;
        }
    }
    return 1;
}
typedef struct {
    WkyDescriptor *parent;
    int mark_roots, dropping;
} ValueCheck;
static int value_check(WkyRef *slot, void *context) {
    ValueCheck *check=context;
    if (!slot->descriptor) return 1;
    WkyDescriptor *d=whole_owner(slot);
    if (d->parent!=check->parent || d->owner_slot!=(check->parent ? slot : NULL))
        fail("unregistered or duplicated owning value");
    /* Root owner_slot is otherwise NULL. Marking it detects duplicate roots
     * in linear time, without allocating or enlarging descriptors. It is
     * cleared before any fallible clone work or committed transfer. */
    if (!check->parent && check->mark_roots) d->owner_slot=slot;
    if (check->dropping) preflight(d);
    return 1;
}
static int value_unmark(WkyRef *slot, void *context) {
    (void)context;
    if (slot->descriptor && !slot->descriptor->parent) slot->descriptor->owner_slot=NULL;
    return 1;
}
static WkyDescriptor *value_container(void *slot, const WkyValueLayout *layout,
                                      const WkyRef *container, int mutate) {
    if (!container) return NULL;
    __wky_mem_write_address(container,slot,layout->size);
    if (mutate) ancestors_unpinned(container->descriptor);
    return container->descriptor;
}
static void value_disjoint(void *first, void *second, uint64_t size) {
    uintptr_t a=(uintptr_t)first, b=(uintptr_t)second;
    if (size && (a>b ? a-b<size : b-a<size)) fail("overlapping owning values");
}
static int value_dispose(WkyRef *slot, void *context) {
    (void)context;
    if (slot->descriptor) dispose_tree(slot->descriptor);
    *slot=(WkyRef){0};
    return 1;
}
void __wky_mem_value_clear(void *slot, const WkyValueLayout *layout, const WkyRef *container) {
    ValueCheck check={.parent=value_container(slot,layout,container,1),.mark_roots=1,.dropping=1};
    value_walk(slot,layout,0,value_check,&check);
    value_walk(slot,layout,1,value_dispose,NULL);
    memset(slot,0,layout->size);
}
void __wky_mem_value_drop(void *slot, const WkyValueLayout *layout) {
    __wky_mem_value_clear(slot,layout,NULL);
}
static int value_detach(WkyRef *slot, void *context) {
    (void)context;
    if (slot->descriptor) detach(slot->descriptor);
    return 1;
}
void __wky_mem_value_take(void *out, void *slot, const WkyValueLayout *layout,
                           const WkyRef *container) {
    value_disjoint(out,slot,layout->size);
    ValueCheck check={.parent=value_container(slot,layout,container,1),.mark_roots=1};
    value_walk(slot,layout,0,value_check,&check);
    value_walk(slot,layout,0,value_detach,NULL);
    memcpy(out,slot,layout->size);
    memset(slot,0,layout->size);
}
typedef struct { WkyDescriptor *parent; } ValueIncoming;
static int value_incoming(WkyRef *slot, void *context) {
    ValueIncoming *check=context;
    if (!slot->descriptor) return 1;
    WkyDescriptor *d=whole_owner(slot);
    if (d->parent || d->owner_slot) fail("owner must be moved before transfer");
    if (check->parent) {
        if (d->arena) fail("arenas cannot be embedded in owning slots");
        for (WkyDescriptor *p=check->parent; p; p=p->parent)
            if (p==d) fail("ownership cycle");
    }
    d->owner_slot=slot;
    return 1;
}
static int value_install(WkyRef *slot, void *context) {
    WkyDescriptor *parent=context;
    if (slot->descriptor) {
        slot->descriptor->owner_slot=NULL;
        if (parent) attach(slot->descriptor,parent,slot,0);
    }
    return 1;
}
void __wky_mem_value_store(void *slot, void *incoming, const WkyValueLayout *layout,
                            const WkyRef *container) {
    value_disjoint(slot,incoming,layout->size);
    WkyDescriptor *parent=value_container(slot,layout,container,1);
    ValueIncoming next={parent};
    value_walk(incoming,layout,0,value_incoming,&next);
    ValueCheck old={.parent=parent,.dropping=1};
    value_walk(slot,layout,0,value_check,&old);
    value_walk(slot,layout,1,value_dispose,NULL);
    memcpy(slot,incoming,layout->size);
    memset(incoming,0,layout->size);
    value_walk(slot,layout,0,value_install,parent);
}
static int value_zero(WkyRef *slot, void *context) {
    (void)context;
    *slot=(WkyRef){0};
    return 1;
}
typedef struct { unsigned char *source, *out; } ValueClone;
static int value_clone(WkyRef *slot, void *context) {
    ValueClone *copy=context;
    WkyRef *source=(WkyRef *)(copy->source+((unsigned char *)slot-copy->out));
    return !source->descriptor || __wky_mem_clone(slot,source);
}
int32_t __wky_mem_value_clone(void *out, void *source, const WkyValueLayout *layout,
                              const WkyRef *container) {
    value_disjoint(out,source,layout->size);
    ValueCheck check={.parent=value_container(source,layout,container,0),.mark_roots=1};
    value_walk(source,layout,0,value_check,&check);
    value_walk(source,layout,0,value_unmark,NULL);
    memcpy(out,source,layout->size);
    value_walk(out,layout,0,value_zero,NULL);
    ValueClone copy={source,out};
    if (value_walk(out,layout,0,value_clone,&copy)) return 1;
    __wky_mem_value_drop(out,layout);
    return 0;
}
/* A length change invalidates every previous view, including views into the
 * retained prefix. Capacity grows geometrically; shrinking retains capacity.
 * Prepare all fallible work before committing either identity or payload.
 * realloc's internal copy/peak is allocator-dependent and is not fabricated
 * as a byte metric; live_bytes counts retained, requested payload capacity. */
int32_t __wky_mem_resize(WkyRef *r, uint64_t count, uint64_t size) {
    WkyDescriptor *d = validate(r);
    if (d->arena || d->parent || r->offset || r->extent != d->bytes)
        fail("resize requires a whole, independently owned allocation");
    if (d->pins) fail("invalidation during stable access");
    if (!size || count > SIZE_MAX / size) return 0;
    uint64_t bytes = count * size;
    if (bytes == d->bytes) return 1;
    /* Shrink drops only owners whose headers leave the visible prefix.
     * All protected descendants are checked before any fallible work or
     * mutation. Retained child allocations keep their independent identity. */
    for (WkyDescriptor *child=d->first; child; child=child->next) {
        uint64_t position=(uintptr_t)child->owner_slot-(uintptr_t)d->data;
        if (position>=bytes) preflight(child);
        else if (sizeof(WkyRef)>bytes-position) fail("resize cuts an owning slot");
    }
    uint64_t capacity = d->capacity;
    if (bytes > capacity) {
        if (!capacity) capacity = size;
        while (capacity < bytes) {
            if (capacity > SIZE_MAX / 2) { capacity = bytes; break; }
            capacity *= 2;
        }
    }
    uint64_t increase = capacity - d->capacity;
    if (increase > budget || increase > UINT64_MAX - live_bytes) return 0;
    uint64_t generation = atomic_load_explicit(&d->generation, memory_order_relaxed);
    WkyDescriptor *replacement = generation == WKY_GENERATION_MAX ? slot_new() : NULL;
    if (generation == WKY_GENERATION_MAX && !replacement) return 0;
    void *data = d->data;
    uintptr_t previous_data=(uintptr_t)data;
    if (increase) {
        data = realloc(data, (size_t)capacity);
        if (!data) { if (replacement) slot_release(replacement); return 0; }
        ++reallocations;
    }
    if (bytes > d->bytes) memset((unsigned char *)data + d->bytes, 0, (size_t)(bytes - d->bytes));
    for (WkyDescriptor *child=d->first; child;) {
        WkyDescriptor *next=child->next;
        uint64_t position=(uintptr_t)child->owner_slot-previous_data;
        if (position>=bytes) dispose_tree(child);
        else child->owner_slot=(WkyRef *)((unsigned char *)data+position);
        child=next;
    }
    if (replacement) {
        replacement->data = data;
        replacement->bytes = bytes;
        replacement->capacity = capacity;
        replacement->pins = replacement->arena = 0;
        replacement->owner_slot = NULL;
        replacement->parent = replacement->first = replacement->last = replacement->previous = replacement->next = NULL;
        replacement->first=d->first; replacement->last=d->last;
        for (WkyDescriptor *child=d->first; child; child=child->next)
            child->parent=replacement;
        d->first=d->last=NULL;
        atomic_store_explicit(&replacement->thread, current_thread(), memory_order_release);
        d->data = NULL;
        slot_release(d);
        d = replacement;
        generation = atomic_load_explicit(&d->generation, memory_order_relaxed);
    } else {
        ++generation;
        atomic_store_explicit(&d->generation, generation, memory_order_release);
        d->data = data; d->bytes = bytes; d->capacity = capacity;
    }
    *r = (WkyRef){d, generation, 0, bytes};
    if (budget != UINT64_MAX) budget -= increase;
    live_bytes += increase;
    if (live_bytes > peak_bytes) peak_bytes = live_bytes;
    ++invalidations;
    return 1;
}
uint64_t __wky_mem_capacity(const WkyRef *r) { return validate(r)->capacity; }
__attribute__((always_inline)) void *__wky_mem_address(WkyDescriptor *d, uint64_t generation,
                         uint64_t offset, uint64_t extent, uint64_t index, uint64_t size) {
    COUNT(checks);
    WkyRef r = {d, generation, offset, extent};
    d = validate(&r);
    if (!size || index >= extent / size) fail("reference index out of bounds");
    return (unsigned char *)d->data + offset + index * size;
}
void __wky_mem_slice(WkyRef *out, const WkyRef *r, uint64_t start, uint64_t end, uint64_t size) {
    validate(r);
    if (!size || start > end || end > r->extent / size) fail("reference slice out of bounds");
    *out = (WkyRef){r->descriptor, r->generation, r->offset + start * size, (end-start)*size};
}
void *__wky_mem_try_pin(const WkyRef *r) {
    if (!alive(r)) return NULL;
    WkyDescriptor *d = validate(r);
    if (d->pins == UINT64_MAX) fail("stable guard count exhausted");
    ++d->pins;
    COUNT(pins);
    return (unsigned char *)d->data + r->offset;
}
void *__wky_mem_pin(const WkyRef *r) {
    void *data = __wky_mem_try_pin(r);
    if (!data) fail("stale reference or foreign thread");
    return data;
}
void __wky_mem_unpin(const WkyRef *r) {
    WkyDescriptor *d = validate(r);
    if (!d->pins) fail("unbalanced stable guard");
    --d->pins;
}
int32_t __wky_mem_arena(WkyRef *out) { return allocate(out, 0, 1, 1); }
int32_t __wky_mem_arena_alloc(WkyRef *out, const WkyRef *r, uint64_t count, uint64_t size) {
    WkyDescriptor *arena = validate(r);
    if (!arena->arena) fail("allocation requires an arena");
    if (!allocate(out, count, size, 0)) return 0;
    WkyDescriptor *d = out->descriptor;
    attach(d, arena, NULL, 0);
    return 1;
}
void __wky_mem_remove(WkyRef *r) {
    WkyDescriptor *d = validate(r);
    if (!d->parent || !d->parent->arena || d->owner_slot) fail("remove requires an arena allocation");
    __wky_mem_drop(r);
}
void __wky_mem_budget(uint64_t bytes) { budget = bytes; }
uint64_t __wky_mem_metric(uint32_t metric) {
    switch (metric) {
    case WKY_MEM_ALLOCATIONS: return allocations;
    case WKY_MEM_FREES: return frees;
    case WKY_MEM_LIVE_BYTES: return live_bytes;
    case WKY_MEM_PEAK_BYTES: return peak_bytes;
    case WKY_MEM_DESCRIPTOR_BYTES: return atomic_load_explicit(&descriptor_bytes, memory_order_relaxed);
    case WKY_MEM_CLONED_BYTES: return cloned_bytes;
    case WKY_MEM_RETIRED_SLOTS: return atomic_load_explicit(&retired_slots, memory_order_relaxed);
    case WKY_MEM_REALLOCATIONS: return reallocations;
    case WKY_MEM_INVALIDATIONS: return invalidations;
#ifdef WKY_MEMORY_METRICS
    case WKY_MEM_CHECKS: return checks;
    case WKY_MEM_PINS: return pins;
    case WKY_MEM_VIEWS: return views;
#endif
    default: return UINT64_MAX; /* unavailable, not a fabricated zero */
    }
}
