/*
 * Dynamic array (type-generic via macros)
 *
 * Usage:
 *   Vec(int) numbers = {0};
 *   vec_push(&numbers, 42);
 *   vec_push(&numbers, 100);
 *   for (size_t i = 0; i < numbers.len; i++) {
 *       printf("%d\n", numbers.data[i]);
 *   }
 *   vec_free(&numbers);
 */

#ifndef MOT_VEC_H
#define MOT_VEC_H

#include <stddef.h>
#include <stdlib.h>
#include <string.h>

/* Generic vector type */
#define Vec(T) struct { T *data; size_t len; size_t cap; }

/* Initialize empty vector */
#define vec_init(v) do { \
    (v)->data = NULL; \
    (v)->len = 0; \
    (v)->cap = 0; \
} while(0)

/* Free vector memory */
#define vec_free(v) do { \
    free((v)->data); \
    (v)->data = NULL; \
    (v)->len = 0; \
    (v)->cap = 0; \
} while(0)

/* Clear vector (keep capacity) */
#define vec_clear(v) do { \
    (v)->len = 0; \
} while(0)

/* Ensure capacity for at least n elements */
#define vec_reserve(v, n) do { \
    if ((n) > (v)->cap) { \
        size_t _new_cap = (v)->cap ? (v)->cap * 2 : 8; \
        while (_new_cap < (n)) _new_cap *= 2; \
        void *_new_data = realloc((v)->data, _new_cap * sizeof(*(v)->data)); \
        if (_new_data) { \
            (v)->data = _new_data; \
            (v)->cap = _new_cap; \
        } \
    } \
} while(0)

/* Push element to end */
#define vec_push(v, elem) do { \
    vec_reserve((v), (v)->len + 1); \
    (v)->data[(v)->len++] = (elem); \
} while(0)

/* Pop element from end (returns the element) */
#define vec_pop(v) ((v)->data[--(v)->len])

/* Get last element */
#define vec_last(v) ((v)->data[(v)->len - 1])

/* Get element at index */
#define vec_get(v, i) ((v)->data[i])

/* Set element at index */
#define vec_set(v, i, elem) ((v)->data[i] = (elem))

/* Insert element at index */
#define vec_insert(v, i, elem) do { \
    vec_reserve((v), (v)->len + 1); \
    memmove(&(v)->data[(i) + 1], &(v)->data[i], \
            ((v)->len - (i)) * sizeof(*(v)->data)); \
    (v)->data[i] = (elem); \
    (v)->len++; \
} while(0)

/* Remove element at index */
#define vec_remove(v, i) do { \
    memmove(&(v)->data[i], &(v)->data[(i) + 1], \
            ((v)->len - (i) - 1) * sizeof(*(v)->data)); \
    (v)->len--; \
} while(0)

/* Iterate over vector */
#define vec_foreach(v, iter) \
    for (size_t iter = 0; iter < (v)->len; iter++)

/* Iterate with pointer */
#define vec_foreach_ptr(v, ptr) \
    for (__typeof__((v)->data) ptr = (v)->data; \
         ptr < (v)->data + (v)->len; ptr++)

#endif /* MOT_VEC_H */
