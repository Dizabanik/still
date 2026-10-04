#include "kawa_memory.h"
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <pthread.h>

/* Metadata pages are process-lived and reused. A stale reference may always
 * examine its descriptor, even after its arena and allocation are destroyed.
 * Retiring exhausted slots avoids ABA; the small counter test build exercises
 * the otherwise impractical exhaustion path. */
#ifndef KAWA_GENERATION_MAX
#define KAWA_GENERATION_MAX UINT64_MAX
#endif
#define PAGE_SLOTS 256
struct KawaDescriptor {
    _Atomic uint64_t thread;
    _Atomic uint64_t generation;
    void *data;
    uint64_t bytes, capacity, pins;
    unsigned arena;
    KawaRef *owner_slot;
    KawaDescriptor *parent, *first, *last, *previous, *next, *free_next;
};
typedef struct Page { struct Page *next; KawaDescriptor slots[PAGE_SLOTS]; } Page;
static Page *pages;
static KawaDescriptor *free_slots;
static pthread_mutex_t pool_lock = PTHREAD_MUTEX_INITIALIZER;
static _Atomic uint64_t next_thread = 1;
static _Atomic uint64_t descriptor_bytes, retired_slots;
static _Thread_local uint64_t thread_id;
static _Thread_local uint64_t budget = UINT64_MAX;
static _Thread_local uint64_t allocations, frees, live_bytes, peak_bytes, cloned_bytes;
static _Thread_local uint64_t reallocations, invalidations;
#ifdef KAWA_MEMORY_METRICS
static _Thread_local uint64_t checks, pins;
#define COUNT(x) (++(x))
#else
#define COUNT(x) ((void)0)
#endif

_Noreturn static void fail(const char *message) {
    fprintf(stderr, "Kawa memory trap: %s\n", message);
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
static KawaDescriptor *slot_new(void) {
    pthread_mutex_lock(&pool_lock);
    if (!free_slots) {
        Page *page = calloc(1, sizeof(*page));
        if (!page) { pthread_mutex_unlock(&pool_lock); return NULL; }
        page->next = pages;
        pages = page;
        atomic_fetch_add_explicit(&descriptor_bytes, sizeof(*page), memory_order_relaxed);
        for (unsigned i = 0; i < PAGE_SLOTS; ++i) {
            KawaDescriptor *d = &page->slots[i];
            atomic_init(&d->thread, 0);
            atomic_init(&d->generation, 1);
            d->free_next = free_slots;
            free_slots = d;
        }
    }
    KawaDescriptor *d = free_slots;
    free_slots = d->free_next;
    pthread_mutex_unlock(&pool_lock);
    return d;
}
static void slot_release(KawaDescriptor *d) {
    uint64_t generation = atomic_load_explicit(&d->generation, memory_order_relaxed);
    atomic_store_explicit(&d->thread, 0, memory_order_release);
    if (generation == KAWA_GENERATION_MAX) {
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
static int alive(const KawaRef *r) {
    KawaDescriptor *d = r->descriptor;
    return d && atomic_load_explicit(&d->thread, memory_order_acquire) == current_thread()
        && atomic_load_explicit(&d->generation, memory_order_acquire) == r->generation;
}
static KawaDescriptor *validate(const KawaRef *r) {
    if (!alive(r)) fail("stale reference or foreign thread");
    KawaDescriptor *d = r->descriptor;
    if (r->offset > d->bytes || r->extent > d->bytes - r->offset)
        fail("invalid reference extent");
    return d;
}
static int allocate(KawaRef *out, uint64_t count, uint64_t size, unsigned arena) {
    *out = (KawaRef){0};
    if (!size || count > SIZE_MAX / size) return 0;
    uint64_t bytes = count * size;
    if (bytes > budget || bytes > UINT64_MAX - live_bytes) return 0;
    /* Even an empty allocation has a unique identity and a non-null address. */
    void *data = calloc(1, bytes ? (size_t)bytes : 1);
    if (!data) return 0;
    KawaDescriptor *d = slot_new();
    if (!d) { free(data); return 0; }
    d->data = data;
    d->bytes = bytes;
    d->capacity = bytes;
    d->pins = 0;
    d->arena = arena;
    d->owner_slot = NULL;
    d->parent = d->first = d->last = d->previous = d->next = NULL;
    atomic_store_explicit(&d->thread, current_thread(), memory_order_release);
    *out = (KawaRef){ d, atomic_load_explicit(&d->generation, memory_order_relaxed), 0, bytes };
    if (budget != UINT64_MAX) budget -= bytes;
    ++allocations;
    live_bytes += bytes;
    if (live_bytes > peak_bytes) peak_bytes = live_bytes;
    return 1;
}
int32_t __kawa_mem_alloc(KawaRef *out, uint64_t count, uint64_t size) {
    return allocate(out, count, size, 0);
}
static void detach(KawaDescriptor *d) {
    if (d->previous) d->previous->next = d->next;
    else if (d->parent) d->parent->first = d->next;
    if (d->next) d->next->previous = d->previous;
    else if (d->parent) d->parent->last = d->previous;
    d->parent = d->previous = d->next = NULL;
    d->owner_slot = NULL;
}
static void attach(KawaDescriptor *d, KawaDescriptor *parent, KawaRef *slot, int last) {
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
static void dispose(KawaDescriptor *d) {
    if (d->owner_slot) *d->owner_slot = (KawaRef){0};
    detach(d);
    free(d->data);
    live_bytes -= d->capacity;
    ++frees;
    d->data = NULL;
    slot_release(d);
}
static void ancestors_unpinned(KawaDescriptor *d) {
    for (; d; d = d->parent)
        if (d->pins) fail(d->arena ? "arena invalidation during stable access" :
                                     "invalidation during stable access");
}
/* The ownership forest doubles as drop/clone metadata. Plain allocations
 * have no children and retain their fast path. Traversal needs no recursive
 * C stack, per-element bitmap, or reference counting. */
static void preflight(KawaDescriptor *root) {
    KawaDescriptor *d = root;
    for (;;) {
        if (d->pins) fail(root->arena ? "arena invalidation during stable access" :
                                        "invalidation during stable access");
        if (d->first) { d = d->first; continue; }
        while (d != root && !d->next) d = d->parent;
        if (d == root) return;
        d = d->next;
    }
}
static void dispose_tree(KawaDescriptor *root) {
    KawaDescriptor *d = root;
    for (;;) {
        if (d->first) { d = d->first; continue; }
        KawaDescriptor *parent = d->parent;
        int done = d == root;
        dispose(d);
        if (done) return;
        d = parent;
    }
}
static KawaDescriptor *whole_owner(const KawaRef *r) {
    KawaDescriptor *d = validate(r);
    if (r->offset || r->extent != d->bytes)
        fail("operation requires a whole owner");
    return d;
}
void *__kawa_mem_write_address(const KawaRef *container, void *slot, uint64_t size) {
    KawaDescriptor *d = validate(container);
    uintptr_t base = (uintptr_t)d->data, address = (uintptr_t)slot;
    if (d->arena || address < base) fail("invalid managed slot");
    uint64_t offset = address - base;
    if (offset < container->offset || offset - container->offset > container->extent ||
        size > container->extent - (offset - container->offset))
        fail("managed slot outside reference bounds");
    return slot;
}
static KawaDescriptor *owning_slot(KawaRef *slot, const KawaRef *container) {
    __kawa_mem_write_address(container, slot, sizeof(*slot));
    if ((uintptr_t)slot % _Alignof(KawaRef)) fail("invalid owning slot");
    return container->descriptor;
}
void __kawa_mem_drop(KawaRef *r) {
    if (!r->descriptor) return; /* moved-from/failed owner */
    KawaDescriptor *d = whole_owner(r);
    ancestors_unpinned(d->parent);
    preflight(d); /* Check the entire forest before changing any state. */
    dispose_tree(d);
    *r = (KawaRef){0};
}
void __kawa_mem_take(KawaRef *out, KawaRef *slot, const KawaRef *container) {
    KawaDescriptor *parent = owning_slot(slot, container);
    ancestors_unpinned(parent);
    KawaRef value = *slot;
    if (value.descriptor) {
        KawaDescriptor *d = whole_owner(&value);
        if (d->owner_slot != slot || d->parent != parent) fail("unregistered owning slot");
        detach(d);
    }
    *slot = (KawaRef){0};
    *out = value;
}
void __kawa_mem_replace(KawaRef *slot, KawaRef *incoming) {
    if (incoming->descriptor) {
        KawaDescriptor *d=whole_owner(incoming);
        if (d->parent) fail("owner must be moved before transfer");
        if (incoming->descriptor==slot->descriptor) fail("owner cannot be duplicated");
    }
    if (slot->descriptor && whole_owner(slot)->parent) fail("replacement requires a root owner");
    __kawa_mem_drop(slot);
    *slot=*incoming;
    *incoming=(KawaRef){0};
}
void __kawa_mem_store_owner(KawaRef *slot, KawaRef *incoming, const KawaRef *container) {
    KawaDescriptor *parent = owning_slot(slot, container), *next = NULL, *old = NULL;
    ancestors_unpinned(parent);
    if (incoming->descriptor) {
        next = whole_owner(incoming);
        if (next->parent) fail("owner must be moved before transfer");
        if (next->arena) fail("arenas cannot be embedded in owning slots");
        for (KawaDescriptor *d = parent; d; d = d->parent)
            if (d == next) fail("ownership cycle");
    }
    if (slot->descriptor) {
        old = whole_owner(slot);
        if (old->owner_slot != slot || old->parent != parent) fail("unregistered owning slot");
        preflight(old);
    }
    if (old) dispose_tree(old);
    *slot = *incoming;
    *incoming = (KawaRef){0};
    if (next) attach(next, parent, slot, 0);
}
static int child_in_view(KawaDescriptor *d, uint64_t offset, uint64_t extent) {
    if (!d->owner_slot) fail("cannot clone arena ownership");
    uint64_t position = (uintptr_t)d->owner_slot - (uintptr_t)d->parent->data;
    if (position >= offset && position - offset < extent) {
        if (sizeof(KawaRef) > extent - (position - offset)) fail("partial owning slot in clone");
        return 1;
    }
    if (position < offset && offset - position < sizeof(KawaRef))
        fail("partial owning slot in clone");
    return 0;
}
static KawaDescriptor *child_next(KawaDescriptor *d, uint64_t offset, uint64_t extent) {
    while (d && !child_in_view(d, offset, extent)) d = d->next;
    return d;
}
static int copy_allocation(KawaRef *out, KawaDescriptor *source, uint64_t offset, uint64_t extent) {
    if (source->arena) fail("cannot clone an arena");
    if (!allocate(out, extent, 1, 0)) return 0;
    memcpy(out->descriptor->data, (unsigned char *)source->data + offset, extent);
    cloned_bytes += extent;
    /* Clear every copied owning header before any child allocation can fail.
     * Failure cleanup must never acquire ownership of source allocations. */
    for (KawaDescriptor *d = source->first; d; d = d->next) {
        if (!child_in_view(d, offset, extent)) continue;
        uint64_t position = (uintptr_t)d->owner_slot - (uintptr_t)source->data - offset;
        *(KawaRef *)((unsigned char *)out->descriptor->data + position) = (KawaRef){0};
    }
    return 1;
}
int32_t __kawa_mem_clone(KawaRef *out, const KawaRef *r) {
    KawaDescriptor *root = validate(r);
    if (!copy_allocation(out, root, r->offset, r->extent)) return 0;
    KawaDescriptor *source = root, *dest = out->descriptor;
    KawaDescriptor *child = child_next(root->first, r->offset, r->extent);
    while (child) {
        KawaRef copy;
        if (!copy_allocation(&copy, child, 0, child->bytes)) {
            dispose_tree(out->descriptor);
            *out = (KawaRef){0};
            return 0;
        }
        uint64_t position = (uintptr_t)child->owner_slot - (uintptr_t)source->data -
                            (source == root ? r->offset : 0);
        KawaRef *slot = (KawaRef *)((unsigned char *)dest->data + position);
        *slot = copy;
        attach(copy.descriptor, dest, slot, 1); /* Preserve acquisition/drop order. */
        if (child->first) {
            source = child; dest = copy.descriptor; child = child->first;
            continue;
        }
        for (;;) {
            KawaDescriptor *next = child_next(child->next,
                source == root ? r->offset : 0, source == root ? r->extent : source->bytes);
            if (next) { child = next; break; }
            if (source == root) return 1;
            child = source; source = source->parent; dest = dest->parent;
        }
    }
    return 1;
}
/* A length change invalidates every previous view, including views into the
 * retained prefix. Capacity grows geometrically; shrinking retains capacity.
 * Prepare all fallible work before committing either identity or payload.
 * realloc's internal copy/peak is allocator-dependent and is not fabricated
 * as a byte metric; live_bytes counts retained, requested payload capacity. */
int32_t __kawa_mem_resize(KawaRef *r, uint64_t count, uint64_t size) {
    KawaDescriptor *d = validate(r);
    if (d->arena || d->parent || r->offset || r->extent != d->bytes)
        fail("resize requires a whole, independently owned allocation");
    if (d->pins) fail("invalidation during stable access");
    if (!size || count > SIZE_MAX / size) return 0;
    uint64_t bytes = count * size;
    if (bytes == d->bytes) return 1;
    /* Shrink drops only owners whose headers leave the visible prefix.
     * All protected descendants are checked before any fallible work or
     * mutation. Retained child allocations keep their independent identity. */
    for (KawaDescriptor *child=d->first; child; child=child->next) {
        uint64_t position=(uintptr_t)child->owner_slot-(uintptr_t)d->data;
        if (position>=bytes) preflight(child);
        else if (sizeof(KawaRef)>bytes-position) fail("resize cuts an owning slot");
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
    KawaDescriptor *replacement = generation == KAWA_GENERATION_MAX ? slot_new() : NULL;
    if (generation == KAWA_GENERATION_MAX && !replacement) return 0;
    void *data = d->data;
    uintptr_t previous_data=(uintptr_t)data;
    if (increase) {
        data = realloc(data, (size_t)capacity);
        if (!data) { if (replacement) slot_release(replacement); return 0; }
        ++reallocations;
    }
    if (bytes > d->bytes) memset((unsigned char *)data + d->bytes, 0, (size_t)(bytes - d->bytes));
    for (KawaDescriptor *child=d->first; child;) {
        KawaDescriptor *next=child->next;
        uint64_t position=(uintptr_t)child->owner_slot-previous_data;
        if (position>=bytes) dispose_tree(child);
        else child->owner_slot=(KawaRef *)((unsigned char *)data+position);
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
        for (KawaDescriptor *child=d->first; child; child=child->next)
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
    *r = (KawaRef){d, generation, 0, bytes};
    if (budget != UINT64_MAX) budget -= increase;
    live_bytes += increase;
    if (live_bytes > peak_bytes) peak_bytes = live_bytes;
    ++invalidations;
    return 1;
}
uint64_t __kawa_mem_capacity(const KawaRef *r) { return validate(r)->capacity; }
__attribute__((always_inline)) void *__kawa_mem_address(KawaDescriptor *d, uint64_t generation,
                         uint64_t offset, uint64_t extent, uint64_t index, uint64_t size) {
    COUNT(checks);
    KawaRef r = {d, generation, offset, extent};
    d = validate(&r);
    if (!size || index >= extent / size) fail("reference index out of bounds");
    return (unsigned char *)d->data + offset + index * size;
}
void __kawa_mem_slice(KawaRef *out, const KawaRef *r, uint64_t start, uint64_t end, uint64_t size) {
    validate(r);
    if (!size || start > end || end > r->extent / size) fail("reference slice out of bounds");
    *out = (KawaRef){r->descriptor, r->generation, r->offset + start * size, (end-start)*size};
}
void *__kawa_mem_try_pin(const KawaRef *r) {
    if (!alive(r)) return NULL;
    KawaDescriptor *d = validate(r);
    if (d->pins == UINT64_MAX) fail("stable guard count exhausted");
    ++d->pins;
    COUNT(pins);
    return (unsigned char *)d->data + r->offset;
}
void *__kawa_mem_pin(const KawaRef *r) {
    void *data = __kawa_mem_try_pin(r);
    if (!data) fail("stale reference or foreign thread");
    return data;
}
void __kawa_mem_unpin(const KawaRef *r) {
    KawaDescriptor *d = validate(r);
    if (!d->pins) fail("unbalanced stable guard");
    --d->pins;
}
int32_t __kawa_mem_arena(KawaRef *out) { return allocate(out, 0, 1, 1); }
int32_t __kawa_mem_arena_alloc(KawaRef *out, const KawaRef *r, uint64_t count, uint64_t size) {
    KawaDescriptor *arena = validate(r);
    if (!arena->arena) fail("allocation requires an arena");
    if (!allocate(out, count, size, 0)) return 0;
    KawaDescriptor *d = out->descriptor;
    attach(d, arena, NULL, 0);
    return 1;
}
void __kawa_mem_remove(KawaRef *r) {
    KawaDescriptor *d = validate(r);
    if (!d->parent || !d->parent->arena || d->owner_slot) fail("remove requires an arena allocation");
    __kawa_mem_drop(r);
}
void __kawa_mem_budget(uint64_t bytes) { budget = bytes; }
uint64_t __kawa_mem_metric(uint32_t metric) {
    switch (metric) {
    case KAWA_MEM_ALLOCATIONS: return allocations;
    case KAWA_MEM_FREES: return frees;
    case KAWA_MEM_LIVE_BYTES: return live_bytes;
    case KAWA_MEM_PEAK_BYTES: return peak_bytes;
    case KAWA_MEM_DESCRIPTOR_BYTES: return atomic_load_explicit(&descriptor_bytes, memory_order_relaxed);
    case KAWA_MEM_CLONED_BYTES: return cloned_bytes;
    case KAWA_MEM_RETIRED_SLOTS: return atomic_load_explicit(&retired_slots, memory_order_relaxed);
    case KAWA_MEM_REALLOCATIONS: return reallocations;
    case KAWA_MEM_INVALIDATIONS: return invalidations;
#ifdef KAWA_MEMORY_METRICS
    case KAWA_MEM_CHECKS: return checks;
    case KAWA_MEM_PINS: return pins;
#endif
    default: return UINT64_MAX; /* unavailable, not a fabricated zero */
    }
}
