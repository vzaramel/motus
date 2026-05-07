/*
 * Hash table (string keys -> void* values)
 * Uses open addressing with linear probing
 */

#ifndef MOT_HASH_H
#define MOT_HASH_H

#include <stddef.h>
#include <stdint.h>
#include <stdbool.h>
#include "str.h"

typedef struct HashEntry {
    char *key;
    void *value;
    uint32_t hash;
    bool occupied;
    bool deleted;
} HashEntry;

typedef struct HashMap {
    HashEntry *entries;
    size_t capacity;
    size_t count;       /* occupied entries */
    size_t tombstones;  /* deleted entries */
} HashMap;

/* Create a new hash map */
HashMap *hashmap_create(void);

/* Create with initial capacity */
HashMap *hashmap_create_capacity(size_t capacity);

/* Destroy hash map (does not free values) */
void hashmap_destroy(HashMap *map);

/* Insert or update a key-value pair (key is copied) */
bool hashmap_set(HashMap *map, const char *key, void *value);

/* Insert with string view key */
bool hashmap_set_sv(HashMap *map, StrView key, void *value);

/* Get value for key (returns NULL if not found) */
void *hashmap_get(HashMap *map, const char *key);

/* Get value with string view key */
void *hashmap_get_sv(HashMap *map, StrView key);

/* Check if key exists */
bool hashmap_has(HashMap *map, const char *key);

/* Remove key (returns removed value or NULL) */
void *hashmap_remove(HashMap *map, const char *key);

/* Clear all entries */
void hashmap_clear(HashMap *map);

/* Get number of entries */
size_t hashmap_count(HashMap *map);

/* Iterator */
typedef struct {
    HashMap *map;
    size_t index;
} HashMapIter;

/* Start iteration */
HashMapIter hashmap_iter(HashMap *map);

/* Get next key-value pair, returns false when done */
bool hashmap_next(HashMapIter *iter, const char **key, void **value);

/* Hash function (FNV-1a) */
uint32_t hash_string(const char *str, size_t len);

#endif /* MOT_HASH_H */
