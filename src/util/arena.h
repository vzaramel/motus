/*
 * Arena allocator - simple bump allocator with bulk free
 */

#ifndef MOT_ARENA_H
#define MOT_ARENA_H

#include <stddef.h>
#include <stdint.h>

typedef struct ArenaChunk ArenaChunk;

struct ArenaChunk {
    ArenaChunk *next;
    size_t size;
    size_t used;
    uint8_t data[];  /* flexible array member */
};

typedef struct Arena {
    ArenaChunk *head;
    ArenaChunk *current;
    size_t chunk_size;   /* default chunk size */
    size_t total_allocated;
} Arena;

/* Create a new arena with the given default chunk size */
Arena *arena_create(size_t chunk_size);

/* Destroy arena and free all memory */
void arena_destroy(Arena *arena);

/* Allocate memory from the arena (8-byte aligned) */
void *arena_alloc(Arena *arena, size_t size);

/* Allocate zeroed memory */
void *arena_calloc(Arena *arena, size_t count, size_t size);

/* Duplicate a string into the arena */
char *arena_strdup(Arena *arena, const char *str);

/* Duplicate n bytes of a string into the arena (null-terminated) */
char *arena_strndup(Arena *arena, const char *str, size_t n);

/* Reset arena for reuse (keeps allocated chunks) */
void arena_reset(Arena *arena);

/* Get total bytes allocated */
size_t arena_total_allocated(Arena *arena);

#endif /* MOT_ARENA_H */
