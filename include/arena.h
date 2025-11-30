#ifndef KAWA_ARENA_H
#define KAWA_ARENA_H
#include <stddef.h>
#include <stdint.h>

typedef struct {
    uint8_t* buffer;
    size_t size;
    size_t offset;
} Arena;

void arena_init(Arena* a, size_t size);
void* arena_alloc(Arena* a, size_t size);
char* arena_strdup(Arena* a, const char* str);
void arena_free(Arena* a);

#endif
