/*
 * Hash table implementation
 */

#include "hash.h"
#include <stdlib.h>
#include <string.h>

#define INITIAL_CAPACITY 16
#define LOAD_FACTOR_GROW 0.7
#define LOAD_FACTOR_SHRINK 0.2
#define MIN_CAPACITY 16

/* FNV-1a hash */
uint32_t hash_string(const char *str, size_t len) {
    uint32_t hash = 2166136261u;
    for (size_t i = 0; i < len; i++) {
        hash ^= (uint8_t)str[i];
        hash *= 16777619u;
    }
    return hash;
}

HashMap *hashmap_create(void) {
    return hashmap_create_capacity(INITIAL_CAPACITY);
}

HashMap *hashmap_create_capacity(size_t capacity) {
    HashMap *map = malloc(sizeof(HashMap));
    if (!map) return NULL;

    /* Round up to power of 2 */
    size_t cap = MIN_CAPACITY;
    while (cap < capacity) cap *= 2;

    map->entries = calloc(cap, sizeof(HashEntry));
    if (!map->entries) {
        free(map);
        return NULL;
    }

    map->capacity = cap;
    map->count = 0;
    map->tombstones = 0;

    return map;
}

void hashmap_destroy(HashMap *map) {
    if (!map) return;

    /* Free copied keys */
    for (size_t i = 0; i < map->capacity; i++) {
        if (map->entries[i].occupied && !map->entries[i].deleted) {
            free(map->entries[i].key);
        }
    }

    free(map->entries);
    free(map);
}

static bool hashmap_resize(HashMap *map, size_t new_capacity) {
    HashEntry *old_entries = map->entries;
    size_t old_capacity = map->capacity;

    map->entries = calloc(new_capacity, sizeof(HashEntry));
    if (!map->entries) {
        map->entries = old_entries;
        return false;
    }

    map->capacity = new_capacity;
    map->count = 0;
    map->tombstones = 0;

    /* Rehash all entries */
    for (size_t i = 0; i < old_capacity; i++) {
        HashEntry *entry = &old_entries[i];
        if (entry->occupied && !entry->deleted) {
            /* Find new slot */
            uint32_t idx = entry->hash & (new_capacity - 1);
            while (map->entries[idx].occupied) {
                idx = (idx + 1) & (new_capacity - 1);
            }

            map->entries[idx] = *entry;
            map->count++;
        } else if (entry->occupied && entry->deleted) {
            /* Free tombstone keys */
            free(entry->key);
        }
    }

    free(old_entries);
    return true;
}

static HashEntry *find_entry(HashMap *map, const char *key, size_t key_len, uint32_t hash) {
    uint32_t idx = hash & (map->capacity - 1);
    HashEntry *tombstone = NULL;

    for (;;) {
        HashEntry *entry = &map->entries[idx];

        if (!entry->occupied) {
            return tombstone ? tombstone : entry;
        }

        if (entry->deleted) {
            if (!tombstone) tombstone = entry;
        } else if (entry->hash == hash &&
                   strlen(entry->key) == key_len &&
                   memcmp(entry->key, key, key_len) == 0) {
            return entry;
        }

        idx = (idx + 1) & (map->capacity - 1);
    }
}

bool hashmap_set(HashMap *map, const char *key, void *value) {
    if (!map || !key) return false;

    /* Check if we need to grow */
    if ((double)(map->count + map->tombstones + 1) / map->capacity > LOAD_FACTOR_GROW) {
        if (!hashmap_resize(map, map->capacity * 2)) {
            return false;
        }
    }

    size_t key_len = strlen(key);
    uint32_t hash = hash_string(key, key_len);
    HashEntry *entry = find_entry(map, key, key_len, hash);

    bool is_new = !entry->occupied || entry->deleted;

    if (is_new) {
        if (entry->deleted) {
            free(entry->key);
            map->tombstones--;
        }

        entry->key = malloc(key_len + 1);
        if (!entry->key) return false;
        memcpy(entry->key, key, key_len + 1);
        entry->hash = hash;
        entry->occupied = true;
        entry->deleted = false;
        map->count++;
    }

    entry->value = value;
    return true;
}

bool hashmap_set_sv(HashMap *map, StrView key, void *value) {
    if (!map || !key.data) return false;

    /* Check if we need to grow */
    if ((double)(map->count + map->tombstones + 1) / map->capacity > LOAD_FACTOR_GROW) {
        if (!hashmap_resize(map, map->capacity * 2)) {
            return false;
        }
    }

    uint32_t hash = hash_string(key.data, key.len);
    HashEntry *entry = find_entry(map, key.data, key.len, hash);

    bool is_new = !entry->occupied || entry->deleted;

    if (is_new) {
        if (entry->deleted) {
            free(entry->key);
            map->tombstones--;
        }

        entry->key = malloc(key.len + 1);
        if (!entry->key) return false;
        memcpy(entry->key, key.data, key.len);
        entry->key[key.len] = '\0';
        entry->hash = hash;
        entry->occupied = true;
        entry->deleted = false;
        map->count++;
    }

    entry->value = value;
    return true;
}

void *hashmap_get(HashMap *map, const char *key) {
    if (!map || !key) return NULL;

    size_t key_len = strlen(key);
    uint32_t hash = hash_string(key, key_len);
    HashEntry *entry = find_entry(map, key, key_len, hash);

    if (!entry->occupied || entry->deleted) {
        return NULL;
    }

    return entry->value;
}

void *hashmap_get_sv(HashMap *map, StrView key) {
    if (!map || !key.data) return NULL;

    uint32_t hash = hash_string(key.data, key.len);
    HashEntry *entry = find_entry(map, key.data, key.len, hash);

    if (!entry->occupied || entry->deleted) {
        return NULL;
    }

    return entry->value;
}

bool hashmap_has(HashMap *map, const char *key) {
    if (!map || !key) return false;

    size_t key_len = strlen(key);
    uint32_t hash = hash_string(key, key_len);
    HashEntry *entry = find_entry(map, key, key_len, hash);

    return entry->occupied && !entry->deleted;
}

void *hashmap_remove(HashMap *map, const char *key) {
    if (!map || !key) return NULL;

    size_t key_len = strlen(key);
    uint32_t hash = hash_string(key, key_len);
    HashEntry *entry = find_entry(map, key, key_len, hash);

    if (!entry->occupied || entry->deleted) {
        return NULL;
    }

    void *value = entry->value;
    entry->deleted = true;
    entry->value = NULL;
    map->count--;
    map->tombstones++;

    /* Check if we should shrink */
    if (map->capacity > MIN_CAPACITY &&
        (double)map->count / map->capacity < LOAD_FACTOR_SHRINK) {
        hashmap_resize(map, map->capacity / 2);
    }

    return value;
}

void hashmap_clear(HashMap *map) {
    if (!map) return;

    for (size_t i = 0; i < map->capacity; i++) {
        if (map->entries[i].occupied) {
            free(map->entries[i].key);
            map->entries[i].occupied = false;
            map->entries[i].deleted = false;
        }
    }

    map->count = 0;
    map->tombstones = 0;
}

size_t hashmap_count(HashMap *map) {
    return map ? map->count : 0;
}

HashMapIter hashmap_iter(HashMap *map) {
    HashMapIter iter;
    iter.map = map;
    iter.index = 0;
    return iter;
}

bool hashmap_next(HashMapIter *iter, const char **key, void **value) {
    if (!iter || !iter->map) return false;

    while (iter->index < iter->map->capacity) {
        HashEntry *entry = &iter->map->entries[iter->index++];
        if (entry->occupied && !entry->deleted) {
            if (key) *key = entry->key;
            if (value) *value = entry->value;
            return true;
        }
    }

    return false;
}
