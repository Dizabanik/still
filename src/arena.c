#include "arena.h"
#include "timbr.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

void arena_init(Arena *a, size_t size) {
	a->buffer = malloc(size);
	if (!a->buffer) {
		perror("Arena malloc failed");
		exit(1);
	}
	a->size = size;
	a->offset = 0;
}

void *arena_alloc(Arena *a, size_t size) {
	size_t padding = (8 - (a->offset % 8)) % 8;
	if (a->offset + size + padding > a->size) {
		timbr_fat("Fatal: Arena Out of Memory\n");
		exit(1);
	}
	a->offset += padding;
	void *ptr = &a->buffer[a->offset];
	a->offset += size;
	memset(ptr, 0, size); // Zero-init for safety
	return ptr;
}

char *arena_strdup(Arena *a, const char *str) {
	size_t len = strlen(str) + 1;
	char *copy = arena_alloc(a, len);
	memcpy(copy, str, len);
	return copy;
}

void arena_free(Arena *a) { free(a->buffer); }
