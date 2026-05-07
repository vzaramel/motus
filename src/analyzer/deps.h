/*
 * Dependency tracking for Motus
 *
 * Tracks fine-grained dependencies like product.name instead of just product.
 * Used for:
 * - Cache invalidation at the field level
 * - Partial evaluation (static vs dynamic determination)
 * - Optimizing re-renders
 */

#ifndef MOT_DEPS_H
#define MOT_DEPS_H

#include <stdbool.h>
#include <stddef.h>
#include "../util/arena.h"

/* Dependency path segment */
typedef struct DepSegment {
    char *name;              /* Field or variable name */
    bool is_index;           /* True if this is an array index (not a field) */
    int index;               /* Index value if is_index (or -1 for dynamic) */
    struct DepSegment *next;
} DepSegment;

/* A single dependency path (e.g., "user.profile.name") */
typedef struct DepPath {
    DepSegment *segments;    /* Linked list of path segments */
    int segment_count;
    char *string_repr;       /* Cached string form: "user.profile.name" */
    unsigned int hash;       /* For fast comparison */
    struct DepPath *next;    /* For linked list in DepSet */
} DepPath;

/* Set of dependencies */
typedef struct DepSet {
    DepPath *paths;          /* Linked list of dependency paths */
    int count;
    Arena *arena;
} DepSet;

/* Dependency context - manages all dependencies for a compilation unit */
typedef struct DepContext {
    Arena *arena;
    DepSet *current;         /* Currently collecting dependencies */
} DepContext;

/* Create dependency context */
DepContext *dep_context_new(Arena *arena);

/* Create empty dependency set */
DepSet *dep_set_new(Arena *arena);

/* Create a dependency path from a variable name */
DepPath *dep_path_var(DepContext *ctx, const char *name);

/* Extend a path with a field access: path.field */
DepPath *dep_path_field(DepContext *ctx, DepPath *base, const char *field);

/* Extend a path with an index access: path[index] */
DepPath *dep_path_index(DepContext *ctx, DepPath *base, int index);

/* Extend a path with a dynamic index: path[?] */
DepPath *dep_path_dynamic_index(DepContext *ctx, DepPath *base);

/* Add a dependency to a set */
void dep_set_add(DepSet *set, DepPath *path);

/* Merge two dependency sets: result = a ∪ b */
DepSet *dep_set_union(DepContext *ctx, DepSet *a, DepSet *b);

/* Check if set contains a path */
bool dep_set_contains(DepSet *set, DepPath *path);

/* Check if set a is a subset of set b */
bool dep_set_subset(DepSet *a, DepSet *b);

/* Check if path a is a prefix of path b */
bool dep_path_is_prefix(DepPath *a, DepPath *b);

/* Check if two paths might conflict (one affects the other) */
bool dep_path_conflicts(DepPath *a, DepPath *b);

/* Get paths that match a prefix */
DepSet *dep_set_filter_prefix(DepContext *ctx, DepSet *set, DepPath *prefix);

/* Convert path to string (e.g., "user.profile.name") */
const char *dep_path_to_string(DepPath *path);

/* Compare two paths */
bool dep_path_equals(DepPath *a, DepPath *b);

/* Debug: print dependency set */
void dep_set_dump(DepSet *set);

/* Dependency collection helpers */

/* Start collecting dependencies */
void dep_start_collect(DepContext *ctx);

/* Stop collecting and return the set */
DepSet *dep_stop_collect(DepContext *ctx);

/* Record a dependency during collection */
void dep_record(DepContext *ctx, DepPath *path);

/* Check if currently collecting */
bool dep_is_collecting(DepContext *ctx);

#endif /* MOT_DEPS_H */
