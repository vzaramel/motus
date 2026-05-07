/*
 * Arena allocator implementation
 */

#ifdef MOT_WASM_FREESTANDING
/* In freestanding WASM builds, arena is implemented in wasm_vm.c */
#else

#include "arena.h"
#include <stdlib.h>
#include <string.h>

#define ALIGN_UP(x, align) (((x) + (align) - 1) & ~((align) - 1))
#define DEFAULT_ALIGNMENT 8

static ArenaChunk *chunk_create(size_t size) {
    ArenaChunk *chunk = malloc(sizeof(ArenaChunk) + size);
    if (!chunk) return NULL;

    chunk->next = NULL;
    chunk->size = size;
    chunk->used = 0;
    return chunk;
}

Arena *arena_create(size_t chunk_size) {
    Arena *arena = malloc(sizeof(Arena));
    if (!arena) return NULL;

    /* Minimum chunk size */
    if (chunk_size < 4096) {
        chunk_size = 4096;
    }

    arena->chunk_size = chunk_size;
    arena->head = chunk_create(chunk_size);
    arena->current = arena->head;
    arena->total_allocated = 0;

    if (!arena->head) {
        free(arena);
        return NULL;
    }

    return arena;
}

void arena_destroy(Arena *arena) {
    if (!arena) return;

    ArenaChunk *chunk = arena->head;
    while (chunk) {
        ArenaChunk *next = chunk->next;
        free(chunk);
        chunk = next;
    }

    free(arena);
}

void *arena_alloc(Arena *arena, size_t size) {
    if (!arena || size == 0) return NULL;

    /* Align size */
    size = ALIGN_UP(size, DEFAULT_ALIGNMENT);

    /* Check if current chunk has space */
    ArenaChunk *chunk = arena->current;
    if (chunk->used + size > chunk->size) {
        /* Need a new chunk */
        size_t new_size = arena->chunk_size;
        if (size > new_size) {
            new_size = size;  /* Large allocation gets its own chunk */
        }

        ArenaChunk *new_chunk = chunk_create(new_size);
        if (!new_chunk) return NULL;

        chunk->next = new_chunk;
        arena->current = new_chunk;
        chunk = new_chunk;
    }

    void *ptr = chunk->data + chunk->used;
    chunk->used += size;
    arena->total_allocated += size;

    return ptr;
}

void *arena_calloc(Arena *arena, size_t count, size_t size) {
    size_t total = count * size;
    void *ptr = arena_alloc(arena, total);
    if (ptr) {
        memset(ptr, 0, total);
    }
    return ptr;
}

char *arena_strdup(Arena *arena, const char *str) {
    if (!str) return NULL;
    size_t len = strlen(str);
    return arena_strndup(arena, str, len);
}

char *arena_strndup(Arena *arena, const char *str, size_t n) {
    if (!str) return NULL;

    char *dup = arena_alloc(arena, n + 1);
    if (dup) {
        memcpy(dup, str, n);
        dup[n] = '\0';
    }
    return dup;
}

void arena_reset(Arena *arena) {
    if (!arena) return;

    /* Reset all chunks */
    ArenaChunk *chunk = arena->head;
    while (chunk) {
        chunk->used = 0;
        chunk = chunk->next;
    }

    arena->current = arena->head;
    arena->total_allocated = 0;
}

size_t arena_total_allocated(Arena *arena) {
    return arena ? arena->total_allocated : 0;
}

#endif /* MOT_WASM_FREESTANDING */
