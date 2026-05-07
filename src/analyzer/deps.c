/*
 * Dependency tracking implementation
 */

#include "deps.h"
#include <stdio.h>
#include <string.h>

/* FNV-1a hash */
static unsigned int hash_string(const char *str) {
    unsigned int hash = 2166136261u;
    while (*str) {
        hash ^= (unsigned char)*str++;
        hash *= 16777619u;
    }
    return hash;
}

/* Build string representation of path */
static char *build_path_string(Arena *arena, DepSegment *segments) {
    /* First pass: calculate length */
    size_t len = 0;
    for (DepSegment *seg = segments; seg; seg = seg->next) {
        if (seg != segments) len++;  /* dot or bracket */
        if (seg->is_index) {
            len += 10;  /* [123] or [?] */
        } else {
            len += strlen(seg->name);
        }
    }

    /* Allocate and build */
    char *str = arena_alloc(arena, len + 1);
    char *p = str;

    for (DepSegment *seg = segments; seg; seg = seg->next) {
        if (seg->is_index) {
            if (seg->index >= 0) {
                p += sprintf(p, "[%d]", seg->index);
            } else {
                p += sprintf(p, "[?]");
            }
        } else {
            if (seg != segments) *p++ = '.';
            strcpy(p, seg->name);
            p += strlen(seg->name);
        }
    }

    *p = '\0';
    return str;
}

DepContext *dep_context_new(Arena *arena) {
    DepContext *ctx = arena_alloc(arena, sizeof(DepContext));
    ctx->arena = arena;
    ctx->current = NULL;
    return ctx;
}

DepSet *dep_set_new(Arena *arena) {
    DepSet *set = arena_alloc(arena, sizeof(DepSet));
    set->paths = NULL;
    set->count = 0;
    set->arena = arena;
    return set;
}

DepPath *dep_path_var(DepContext *ctx, const char *name) {
    DepPath *path = arena_alloc(ctx->arena, sizeof(DepPath));

    /* Create single segment */
    DepSegment *seg = arena_alloc(ctx->arena, sizeof(DepSegment));
    seg->name = arena_strdup(ctx->arena, name);
    seg->is_index = false;
    seg->index = -1;
    seg->next = NULL;

    path->segments = seg;
    path->segment_count = 1;
    path->string_repr = build_path_string(ctx->arena, seg);
    path->hash = hash_string(path->string_repr);
    path->next = NULL;

    return path;
}

/* Clone a path's segments */
static DepSegment *clone_segments(Arena *arena, DepSegment *src, int *count) {
    DepSegment *head = NULL;
    DepSegment **tail = &head;
    *count = 0;

    while (src) {
        DepSegment *seg = arena_alloc(arena, sizeof(DepSegment));
        seg->name = src->name ? arena_strdup(arena, src->name) : NULL;
        seg->is_index = src->is_index;
        seg->index = src->index;
        seg->next = NULL;

        *tail = seg;
        tail = &seg->next;
        (*count)++;
        src = src->next;
    }

    return head;
}

DepPath *dep_path_field(DepContext *ctx, DepPath *base, const char *field) {
    DepPath *path = arena_alloc(ctx->arena, sizeof(DepPath));

    /* Clone base segments */
    int count;
    path->segments = clone_segments(ctx->arena, base->segments, &count);

    /* Append field segment */
    DepSegment *seg = arena_alloc(ctx->arena, sizeof(DepSegment));
    seg->name = arena_strdup(ctx->arena, field);
    seg->is_index = false;
    seg->index = -1;
    seg->next = NULL;

    /* Find tail */
    DepSegment *tail = path->segments;
    while (tail->next) tail = tail->next;
    tail->next = seg;

    path->segment_count = count + 1;
    path->string_repr = build_path_string(ctx->arena, path->segments);
    path->hash = hash_string(path->string_repr);
    path->next = NULL;

    return path;
}

DepPath *dep_path_index(DepContext *ctx, DepPath *base, int index) {
    DepPath *path = arena_alloc(ctx->arena, sizeof(DepPath));

    int count;
    path->segments = clone_segments(ctx->arena, base->segments, &count);

    DepSegment *seg = arena_alloc(ctx->arena, sizeof(DepSegment));
    seg->name = NULL;
    seg->is_index = true;
    seg->index = index;
    seg->next = NULL;

    DepSegment *tail = path->segments;
    while (tail->next) tail = tail->next;
    tail->next = seg;

    path->segment_count = count + 1;
    path->string_repr = build_path_string(ctx->arena, path->segments);
    path->hash = hash_string(path->string_repr);
    path->next = NULL;

    return path;
}

DepPath *dep_path_dynamic_index(DepContext *ctx, DepPath *base) {
    return dep_path_index(ctx, base, -1);
}

void dep_set_add(DepSet *set, DepPath *path) {
    /* Check if already present */
    for (DepPath *p = set->paths; p; p = p->next) {
        if (dep_path_equals(p, path)) return;
    }

    /* Clone path for this set */
    DepPath *copy = arena_alloc(set->arena, sizeof(DepPath));
    copy->segments = clone_segments(set->arena, path->segments, &copy->segment_count);
    copy->string_repr = arena_strdup(set->arena, path->string_repr);
    copy->hash = path->hash;
    copy->next = set->paths;
    set->paths = copy;
    set->count++;
}

DepSet *dep_set_union(DepContext *ctx, DepSet *a, DepSet *b) {
    DepSet *result = dep_set_new(ctx->arena);

    for (DepPath *p = a->paths; p; p = p->next) {
        dep_set_add(result, p);
    }
    for (DepPath *p = b->paths; p; p = p->next) {
        dep_set_add(result, p);
    }

    return result;
}

bool dep_set_contains(DepSet *set, DepPath *path) {
    for (DepPath *p = set->paths; p; p = p->next) {
        if (dep_path_equals(p, path)) return true;
    }
    return false;
}

bool dep_set_subset(DepSet *a, DepSet *b) {
    for (DepPath *p = a->paths; p; p = p->next) {
        if (!dep_set_contains(b, p)) return false;
    }
    return true;
}

bool dep_path_is_prefix(DepPath *a, DepPath *b) {
    if (a->segment_count > b->segment_count) return false;

    DepSegment *sa = a->segments;
    DepSegment *sb = b->segments;

    while (sa) {
        if (!sb) return false;

        if (sa->is_index != sb->is_index) return false;

        if (sa->is_index) {
            /* Dynamic index matches any index */
            if (sa->index >= 0 && sb->index >= 0 && sa->index != sb->index) {
                return false;
            }
        } else {
            if (strcmp(sa->name, sb->name) != 0) return false;
        }

        sa = sa->next;
        sb = sb->next;
    }

    return true;
}

bool dep_path_conflicts(DepPath *a, DepPath *b) {
    /* Two paths conflict if one is a prefix of the other */
    return dep_path_is_prefix(a, b) || dep_path_is_prefix(b, a);
}

DepSet *dep_set_filter_prefix(DepContext *ctx, DepSet *set, DepPath *prefix) {
    DepSet *result = dep_set_new(ctx->arena);

    for (DepPath *p = set->paths; p; p = p->next) {
        if (dep_path_is_prefix(prefix, p)) {
            dep_set_add(result, p);
        }
    }

    return result;
}

const char *dep_path_to_string(DepPath *path) {
    return path->string_repr;
}

bool dep_path_equals(DepPath *a, DepPath *b) {
    if (a == b) return true;
    if (!a || !b) return false;
    if (a->hash != b->hash) return false;
    return strcmp(a->string_repr, b->string_repr) == 0;
}

void dep_set_dump(DepSet *set) {
    printf("DepSet[%d]:\n", set->count);
    for (DepPath *p = set->paths; p; p = p->next) {
        printf("  - %s\n", p->string_repr);
    }
}

void dep_start_collect(DepContext *ctx) {
    ctx->current = dep_set_new(ctx->arena);
}

DepSet *dep_stop_collect(DepContext *ctx) {
    DepSet *result = ctx->current;
    ctx->current = NULL;
    return result;
}

void dep_record(DepContext *ctx, DepPath *path) {
    if (ctx->current) {
        dep_set_add(ctx->current, path);
    }
}

bool dep_is_collecting(DepContext *ctx) {
    return ctx->current != NULL;
}
