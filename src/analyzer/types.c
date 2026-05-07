/*
 * Type system implementation
 */

#include "types.h"
#include <stdio.h>
#include <string.h>

/* Create a basic type */
static Type *make_type(Arena *arena, TypeKind kind) {
    Type *t = arena_alloc(arena, sizeof(Type));
    t->kind = kind;
    t->hash = 0;
    return t;
}

TypeContext *type_context_new(Arena *arena) {
    TypeContext *ctx = arena_alloc(arena, sizeof(TypeContext));
    ctx->arena = arena;

    /* Create cached primitive types */
    ctx->t_unknown = make_type(arena, TYPE_UNKNOWN);
    ctx->t_any = make_type(arena, TYPE_ANY);
    ctx->t_void = make_type(arena, TYPE_VOID);
    ctx->t_null = make_type(arena, TYPE_NULL);
    ctx->t_bool = make_type(arena, TYPE_BOOL);
    ctx->t_int = make_type(arena, TYPE_INT);
    ctx->t_number = make_type(arena, TYPE_NUMBER);
    ctx->t_string = make_type(arena, TYPE_STRING);

    /* Pre-create common array types */
    ctx->t_string_array = type_array(ctx, ctx->t_string);
    ctx->t_int_array = type_array(ctx, ctx->t_int);
    ctx->t_number_array = type_array(ctx, ctx->t_number);

    return ctx;
}

Type *type_unknown(TypeContext *ctx) { return ctx->t_unknown; }
Type *type_any(TypeContext *ctx) { return ctx->t_any; }
Type *type_void(TypeContext *ctx) { return ctx->t_void; }
Type *type_null(TypeContext *ctx) { return ctx->t_null; }
Type *type_bool(TypeContext *ctx) { return ctx->t_bool; }
Type *type_int(TypeContext *ctx) { return ctx->t_int; }
Type *type_number(TypeContext *ctx) { return ctx->t_number; }
Type *type_string(TypeContext *ctx) { return ctx->t_string; }

Type *type_array(TypeContext *ctx, Type *element) {
    /* Check for cached common arrays */
    if (element == ctx->t_string && ctx->t_string_array) {
        return ctx->t_string_array;
    }
    if (element == ctx->t_int && ctx->t_int_array) {
        return ctx->t_int_array;
    }
    if (element == ctx->t_number && ctx->t_number_array) {
        return ctx->t_number_array;
    }

    Type *t = make_type(ctx->arena, TYPE_ARRAY);
    t->data.array.element = element;
    return t;
}

Type *type_object(TypeContext *ctx, TypeField *fields) {
    Type *t = make_type(ctx->arena, TYPE_OBJECT);
    t->data.object.fields = fields;

    /* Count fields */
    int count = 0;
    for (TypeField *f = fields; f; f = f->next) count++;
    t->data.object.field_count = count;

    return t;
}

Type *type_tuple(TypeContext *ctx, Type **elements, int count) {
    Type *t = make_type(ctx->arena, TYPE_TUPLE);

    /* Build fields without names */
    TypeField *fields = NULL;
    TypeField **tail = &fields;
    for (int i = 0; i < count; i++) {
        TypeField *f = arena_alloc(ctx->arena, sizeof(TypeField));
        f->name = NULL;
        f->type = elements[i];
        f->optional = false;
        f->next = NULL;
        *tail = f;
        tail = &f->next;
    }

    t->data.object.fields = fields;
    t->data.object.field_count = count;
    return t;
}

Type *type_union(TypeContext *ctx, Type **members, int count) {
    /* Simplify single-member unions */
    if (count == 1) return members[0];

    /* Check for T | null -> optional */
    if (count == 2) {
        if (members[0]->kind == TYPE_NULL) {
            return type_optional(ctx, members[1]);
        }
        if (members[1]->kind == TYPE_NULL) {
            return type_optional(ctx, members[0]);
        }
    }

    Type *t = make_type(ctx->arena, TYPE_UNION);
    t->data.tunion.members = arena_alloc(ctx->arena, sizeof(Type*) * count);
    memcpy(t->data.tunion.members, members, sizeof(Type*) * count);
    t->data.tunion.member_count = count;
    return t;
}

Type *type_optional(TypeContext *ctx, Type *inner) {
    /* null? is just null */
    if (inner->kind == TYPE_NULL) return inner;
    /* Optional of optional is just optional */
    if (inner->kind == TYPE_OPTIONAL) return inner;

    Type *t = make_type(ctx->arena, TYPE_OPTIONAL);
    t->data.optional.inner = inner;
    return t;
}

Type *type_function(TypeContext *ctx, Type **params, int param_count,
                    Type *ret, bool variadic) {
    Type *t = make_type(ctx->arena, TYPE_FUNCTION);
    if (param_count > 0) {
        t->data.func.params = arena_alloc(ctx->arena, sizeof(Type*) * param_count);
        memcpy(t->data.func.params, params, sizeof(Type*) * param_count);
    } else {
        t->data.func.params = NULL;
    }
    t->data.func.param_count = param_count;
    t->data.func.ret = ret;
    t->data.func.variadic = variadic;
    return t;
}

TypeField *type_field(TypeContext *ctx, const char *name, Type *type, bool optional) {
    TypeField *f = arena_alloc(ctx->arena, sizeof(TypeField));
    f->name = name ? arena_strdup(ctx->arena, name) : NULL;
    f->type = type;
    f->optional = optional;
    f->next = NULL;
    return f;
}

bool type_equals(Type *a, Type *b) {
    if (a == b) return true;
    if (!a || !b) return false;
    if (a->kind != b->kind) return false;

    switch (a->kind) {
        case TYPE_ARRAY:
            return type_equals(a->data.array.element, b->data.array.element);

        case TYPE_OBJECT:
        case TYPE_TUPLE: {
            if (a->data.object.field_count != b->data.object.field_count) {
                return false;
            }
            TypeField *fa = a->data.object.fields;
            TypeField *fb = b->data.object.fields;
            while (fa && fb) {
                /* For tuples, names are NULL */
                if (fa->name && fb->name && strcmp(fa->name, fb->name) != 0) {
                    return false;
                }
                if (!type_equals(fa->type, fb->type)) return false;
                fa = fa->next;
                fb = fb->next;
            }
            return fa == NULL && fb == NULL;
        }

        case TYPE_UNION: {
            if (a->data.tunion.member_count != b->data.tunion.member_count) {
                return false;
            }
            /* Order-sensitive comparison for now */
            for (int i = 0; i < a->data.tunion.member_count; i++) {
                if (!type_equals(a->data.tunion.members[i], b->data.tunion.members[i])) {
                    return false;
                }
            }
            return true;
        }

        case TYPE_OPTIONAL:
            return type_equals(a->data.optional.inner, b->data.optional.inner);

        case TYPE_FUNCTION: {
            if (a->data.func.param_count != b->data.func.param_count) return false;
            if (a->data.func.variadic != b->data.func.variadic) return false;
            if (!type_equals(a->data.func.ret, b->data.func.ret)) return false;
            for (int i = 0; i < a->data.func.param_count; i++) {
                if (!type_equals(a->data.func.params[i], b->data.func.params[i])) {
                    return false;
                }
            }
            return true;
        }

        /* Primitive types - kind equality is sufficient */
        default:
            return true;
    }
}

bool type_assignable(Type *target, Type *source) {
    /* Same type always works */
    if (type_equals(target, source)) return true;

    /* Any accepts everything */
    if (target->kind == TYPE_ANY) return true;

    /* Unknown can be assigned to anything (pre-analysis) */
    if (source->kind == TYPE_UNKNOWN) return true;

    /* null assignable to optional */
    if (target->kind == TYPE_OPTIONAL && source->kind == TYPE_NULL) {
        return true;
    }

    /* T assignable to T? */
    if (target->kind == TYPE_OPTIONAL) {
        return type_assignable(target->data.optional.inner, source);
    }

    /* int assignable to number (widening) */
    if (target->kind == TYPE_NUMBER && source->kind == TYPE_INT) {
        return true;
    }

    /* Array covariance */
    if (target->kind == TYPE_ARRAY && source->kind == TYPE_ARRAY) {
        return type_assignable(target->data.array.element, source->data.array.element);
    }

    /* Union: source must be assignable to at least one member */
    if (target->kind == TYPE_UNION) {
        for (int i = 0; i < target->data.tunion.member_count; i++) {
            if (type_assignable(target->data.tunion.members[i], source)) {
                return true;
            }
        }
    }

    /* Source union: all members must be assignable to target */
    if (source->kind == TYPE_UNION) {
        for (int i = 0; i < source->data.tunion.member_count; i++) {
            if (!type_assignable(target, source->data.tunion.members[i])) {
                return false;
            }
        }
        return true;
    }

    return false;
}

Type *type_common(TypeContext *ctx, Type *a, Type *b) {
    if (type_equals(a, b)) return a;

    /* Prefer non-null */
    if (a->kind == TYPE_NULL) return type_optional(ctx, b);
    if (b->kind == TYPE_NULL) return type_optional(ctx, a);

    /* int + number -> number */
    if ((a->kind == TYPE_INT && b->kind == TYPE_NUMBER) ||
        (a->kind == TYPE_NUMBER && b->kind == TYPE_INT)) {
        return ctx->t_number;
    }

    /* Array element common type */
    if (a->kind == TYPE_ARRAY && b->kind == TYPE_ARRAY) {
        Type *elem = type_common(ctx, a->data.array.element, b->data.array.element);
        return type_array(ctx, elem);
    }

    /* Fall back to union */
    Type *members[2] = {a, b};
    return type_union(ctx, members, 2);
}

bool type_is_numeric(Type *t) {
    return t->kind == TYPE_INT || t->kind == TYPE_NUMBER;
}

bool type_is_iterable(Type *t) {
    return t->kind == TYPE_ARRAY || t->kind == TYPE_STRING;
}

Type *type_element_of(Type *t) {
    if (t->kind == TYPE_ARRAY) {
        return t->data.array.element;
    }
    return NULL;
}

Type *type_field_type(Type *t, const char *field) {
    if (t->kind != TYPE_OBJECT) return NULL;

    for (TypeField *f = t->data.object.fields; f; f = f->next) {
        if (f->name && strcmp(f->name, field) == 0) {
            return f->type;
        }
    }
    return NULL;
}

const char *type_to_string(TypeContext *ctx, Type *type) {
    if (!type) return "null";

    /* Use a small static buffer for simple types */
    static char buf[256];

    switch (type->kind) {
        case TYPE_UNKNOWN: return "unknown";
        case TYPE_ANY: return "any";
        case TYPE_VOID: return "void";
        case TYPE_NULL: return "null";
        case TYPE_BOOL: return "bool";
        case TYPE_INT: return "int";
        case TYPE_NUMBER: return "number";
        case TYPE_STRING: return "string";

        case TYPE_ARRAY:
            snprintf(buf, sizeof(buf), "array<%s>",
                     type_to_string(ctx, type->data.array.element));
            return buf;

        case TYPE_OPTIONAL:
            snprintf(buf, sizeof(buf), "%s?",
                     type_to_string(ctx, type->data.optional.inner));
            return buf;

        case TYPE_OBJECT: {
            char *p = buf;
            char *end = buf + sizeof(buf) - 1;
            *p++ = '{';
            for (TypeField *f = type->data.object.fields; f && p < end - 10; f = f->next) {
                if (f != type->data.object.fields) {
                    *p++ = ',';
                    *p++ = ' ';
                }
                p += snprintf(p, end - p, "%s: %s",
                              f->name ? f->name : "?",
                              type_to_string(ctx, f->type));
            }
            *p++ = '}';
            *p = '\0';
            return buf;
        }

        case TYPE_TUPLE: {
            char *p = buf;
            char *end = buf + sizeof(buf) - 1;
            *p++ = '(';
            int i = 0;
            for (TypeField *f = type->data.object.fields; f && p < end - 10; f = f->next) {
                if (i++ > 0) {
                    *p++ = ',';
                    *p++ = ' ';
                }
                p += snprintf(p, end - p, "%s", type_to_string(ctx, f->type));
            }
            *p++ = ')';
            *p = '\0';
            return buf;
        }

        case TYPE_UNION: {
            char *p = buf;
            char *end = buf + sizeof(buf) - 1;
            for (int i = 0; i < type->data.tunion.member_count && p < end - 10; i++) {
                if (i > 0) {
                    p += snprintf(p, end - p, " | ");
                }
                p += snprintf(p, end - p, "%s",
                              type_to_string(ctx, type->data.tunion.members[i]));
            }
            return buf;
        }

        case TYPE_FUNCTION: {
            char *p = buf;
            char *end = buf + sizeof(buf) - 1;
            *p++ = '(';
            for (int i = 0; i < type->data.func.param_count && p < end - 10; i++) {
                if (i > 0) {
                    *p++ = ',';
                    *p++ = ' ';
                }
                p += snprintf(p, end - p, "%s",
                              type_to_string(ctx, type->data.func.params[i]));
            }
            if (type->data.func.variadic && p < end - 5) {
                p += snprintf(p, end - p, "...");
            }
            p += snprintf(p, end - p, ") -> %s",
                          type_to_string(ctx, type->data.func.ret));
            return buf;
        }

        case TYPE_COMPONENT:
            snprintf(buf, sizeof(buf), "component<%s>",
                     type->data.component.name ? type->data.component.name : "?");
            return buf;

        case TYPE_SLOT:
            return "slot";
    }

    return "?";
}
