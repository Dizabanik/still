#ifndef WKY_MEMORY_H
#define WKY_MEMORY_H
#include <stdint.h>
#include <stddef.h>

/* Compiler/runtime ABI. The descriptor is opaque to source programs. All
 * extents and offsets are bytes. No ABI-dependent struct returns are used. */
typedef struct WkyDescriptor WkyDescriptor;
typedef struct {
    WkyDescriptor *descriptor;
    uint64_t generation, offset, extent;
} WkyRef;
/* Static compiler layouts describe embedded owner headers, including nested
 * fixed arrays. NULL field.layout denotes one owner header. They never
 * describe borrowed ref fields or an owner's separately allocated payload. */
typedef struct WkyValueLayout WkyValueLayout;
typedef struct {
    uint64_t offset, count, stride;
    const WkyValueLayout *layout;
    int64_t tag;
    uint64_t conditional; /* select this field only for the active enum tag */
} WkyOwnedField;
struct WkyValueLayout {
    uint64_t size, count;
    const WkyOwnedField *fields;
};
void __wky_mem_value_drop(void *slot, const WkyValueLayout *);
void __wky_mem_value_clear(void *slot, const WkyValueLayout *, const WkyRef *container);
void __wky_mem_value_take(void *out, void *slot, const WkyValueLayout *,
                           const WkyRef *container);
void __wky_mem_value_store(void *slot, void *incoming, const WkyValueLayout *,
                            const WkyRef *container);
int32_t __wky_mem_value_clone(void *out, void *source, const WkyValueLayout *,
                              const WkyRef *container);

int32_t __wky_mem_alloc(WkyRef *out, uint64_t count, uint64_t size);
/* Adopt an opaque resource. Its callback releases the external allocation;
 * descriptor lifetime, transfer, pinning and recursive drop remain shared. */
int32_t __wky_mem_adopt(WkyRef *out, void *resource, void (*destroy)(void *));
void __wky_mem_drop(WkyRef *owner);
void __wky_mem_replace(WkyRef *slot, WkyRef *incoming);
/* Transfer a whole owner into/out of managed storage. Validate the container
 * before reading a slot, including after a callback invalidates its storage. */
void __wky_mem_store_owner(WkyRef *slot, WkyRef *incoming, const WkyRef *container);
void __wky_mem_take(WkyRef *out, WkyRef *slot, const WkyRef *container);
void *__wky_mem_write_address(const WkyRef *container, void *slot, uint64_t size);
void __wky_mem_view(WkyRef *out,const WkyRef *container,void *slot,uint64_t size);
int32_t __wky_mem_clone(WkyRef *out, const WkyRef *source);
int32_t __wky_mem_resize(WkyRef *owner, uint64_t count, uint64_t size);
uint64_t __wky_mem_capacity(const WkyRef *ref);
void *__wky_mem_address(WkyDescriptor *, uint64_t generation,
                         uint64_t offset, uint64_t extent,
                         uint64_t index, uint64_t size);
void __wky_mem_slice(WkyRef *out, const WkyRef *, uint64_t start,
                      uint64_t end, uint64_t size);
void *__wky_mem_pin(const WkyRef *ref);
void *__wky_mem_try_pin(const WkyRef *ref);
void __wky_mem_unpin(const WkyRef *ref);
int32_t __wky_mem_arena(WkyRef *out);
int32_t __wky_mem_arena_alloc(WkyRef *out, const WkyRef *arena,
                              uint64_t count, uint64_t size);
void __wky_mem_remove(WkyRef *ref);
void __wky_mem_budget(uint64_t bytes);
uint64_t __wky_mem_metric(uint32_t metric);

enum {
    WKY_MEM_ALLOCATIONS, WKY_MEM_FREES, WKY_MEM_LIVE_BYTES,
    WKY_MEM_PEAK_BYTES, WKY_MEM_DESCRIPTOR_BYTES, WKY_MEM_CLONED_BYTES,
    WKY_MEM_RETIRED_SLOTS, WKY_MEM_CHECKS, WKY_MEM_PINS,
    WKY_MEM_REALLOCATIONS, WKY_MEM_INVALIDATIONS, WKY_MEM_VIEWS
};
#endif
