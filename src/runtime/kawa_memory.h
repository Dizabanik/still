#ifndef KAWA_MEMORY_H
#define KAWA_MEMORY_H
#include <stdint.h>
#include <stddef.h>

/* Compiler/runtime ABI. The descriptor is opaque to source programs. All
 * extents and offsets are bytes. No ABI-dependent struct returns are used. */
typedef struct KawaDescriptor KawaDescriptor;
typedef struct {
    KawaDescriptor *descriptor;
    uint64_t generation, offset, extent;
} KawaRef;
/* Static compiler layouts describe embedded owner headers, including nested
 * fixed arrays. NULL field.layout denotes one owner header. They never
 * describe borrowed ref fields or an owner's separately allocated payload. */
typedef struct KawaValueLayout KawaValueLayout;
typedef struct {
    uint64_t offset, count, stride;
    const KawaValueLayout *layout;
} KawaOwnedField;
struct KawaValueLayout {
    uint64_t size, count;
    const KawaOwnedField *fields;
};
void __kawa_mem_value_drop(void *slot, const KawaValueLayout *);
void __kawa_mem_value_clear(void *slot, const KawaValueLayout *, const KawaRef *container);
void __kawa_mem_value_take(void *out, void *slot, const KawaValueLayout *,
                           const KawaRef *container);
void __kawa_mem_value_store(void *slot, void *incoming, const KawaValueLayout *,
                            const KawaRef *container);
int32_t __kawa_mem_value_clone(void *out, void *source, const KawaValueLayout *,
                              const KawaRef *container);

int32_t __kawa_mem_alloc(KawaRef *out, uint64_t count, uint64_t size);
void __kawa_mem_drop(KawaRef *owner);
void __kawa_mem_replace(KawaRef *slot, KawaRef *incoming);
/* Transfer a whole owner into/out of managed storage. Validate the container
 * before reading a slot, including after a callback invalidates its storage. */
void __kawa_mem_store_owner(KawaRef *slot, KawaRef *incoming, const KawaRef *container);
void __kawa_mem_take(KawaRef *out, KawaRef *slot, const KawaRef *container);
void *__kawa_mem_write_address(const KawaRef *container, void *slot, uint64_t size);
void __kawa_mem_view(KawaRef *out,const KawaRef *container,void *slot,uint64_t size);
int32_t __kawa_mem_clone(KawaRef *out, const KawaRef *source);
int32_t __kawa_mem_resize(KawaRef *owner, uint64_t count, uint64_t size);
uint64_t __kawa_mem_capacity(const KawaRef *ref);
void *__kawa_mem_address(KawaDescriptor *, uint64_t generation,
                         uint64_t offset, uint64_t extent,
                         uint64_t index, uint64_t size);
void __kawa_mem_slice(KawaRef *out, const KawaRef *, uint64_t start,
                      uint64_t end, uint64_t size);
void *__kawa_mem_pin(const KawaRef *ref);
void *__kawa_mem_try_pin(const KawaRef *ref);
void __kawa_mem_unpin(const KawaRef *ref);
int32_t __kawa_mem_arena(KawaRef *out);
int32_t __kawa_mem_arena_alloc(KawaRef *out, const KawaRef *arena,
                              uint64_t count, uint64_t size);
void __kawa_mem_remove(KawaRef *ref);
void __kawa_mem_budget(uint64_t bytes);
uint64_t __kawa_mem_metric(uint32_t metric);

enum {
    KAWA_MEM_ALLOCATIONS, KAWA_MEM_FREES, KAWA_MEM_LIVE_BYTES,
    KAWA_MEM_PEAK_BYTES, KAWA_MEM_DESCRIPTOR_BYTES, KAWA_MEM_CLONED_BYTES,
    KAWA_MEM_RETIRED_SLOTS, KAWA_MEM_CHECKS, KAWA_MEM_PINS,
    KAWA_MEM_REALLOCATIONS, KAWA_MEM_INVALIDATIONS, KAWA_MEM_VIEWS
};
#endif
