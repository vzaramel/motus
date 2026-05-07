/*
 * Type system for Motus
 */

#ifndef MOT_TYPES_H
#define MOT_TYPES_H

#include <stdbool.h>
#include <stddef.h>
#include "../util/arena.h"

/* Type kinds */
typedef enum {
    TYPE_UNKNOWN,    /* Not yet resolved */
    TYPE_ANY,        /* Dynamic type (skips type checking) */
    TYPE_VOID,       /* No value (for macros/components that only emit) */
    TYPE_NULL,       /* null literal */

    /* Primitives */
    TYPE_BOOL,
    TYPE_INT,
    TYPE_NUMBER,     /* floating point */
    TYPE_STRING,

    /* Composites */
    TYPE_ARRAY,      /* array<element_type> */
    TYPE_OBJECT,     /* { field: type, ... } */
    TYPE_TUPLE,      /* (T1, T2, ...) for SQL rows */

    /* Special */
    TYPE_UNION,      /* T1 | T2 */
    TYPE_OPTIONAL,   /* T? (sugar for T | null) */
    TYPE_FUNCTION,   /* (args) -> return */
    TYPE_COMPONENT,  /* component reference */
    TYPE_SLOT,       /* slot type */
} TypeKind;

/* Forward declarations */
typedef struct Type Type;
typedef struct TypeField TypeField;

/* Object/tuple field */
struct TypeField {
    char *name;          /* Field name (NULL for tuple positional) */
    Type *type;
    bool optional;       /* For optional object fields */
    TypeField *next;
};

/* Type structure */
struct Type {
    TypeKind kind;

    union {
        /* TYPE_ARRAY */
        struct {
            Type *element;
        } array;

        /* TYPE_OBJECT, TYPE_TUPLE */
        struct {
            TypeField *fields;
            int field_count;
        } object;

        /* TYPE_UNION */
        struct {
            Type **members;
            int member_count;
        } tunion;

        /* TYPE_OPTIONAL */
        struct {
            Type *inner;
        } optional;

        /* TYPE_FUNCTION */
        struct {
            Type **params;
            int param_count;
            Type *ret;
            bool variadic;   /* Last param is rest */
        } func;

        /* TYPE_COMPONENT */
        struct {
            char *name;
            TypeField *props;    /* Expected props */
            TypeField *slots;    /* Expected slots */
        } component;
    } data;

    /* Cached hash for type comparison */
    unsigned int hash;
};

/* Type context for interning and caching */
typedef struct TypeContext {
    Arena *arena;

    /* Cached primitive types */
    Type *t_unknown;
    Type *t_any;
    Type *t_void;
    Type *t_null;
    Type *t_bool;
    Type *t_int;
    Type *t_number;
    Type *t_string;

    /* Common array types */
    Type *t_string_array;
    Type *t_int_array;
    Type *t_number_array;
} TypeContext;

/* Create type context */
TypeContext *type_context_new(Arena *arena);

/* Get primitive types */
Type *type_unknown(TypeContext *ctx);
Type *type_any(TypeContext *ctx);
Type *type_void(TypeContext *ctx);
Type *type_null(TypeContext *ctx);
Type *type_bool(TypeContext *ctx);
Type *type_int(TypeContext *ctx);
Type *type_number(TypeContext *ctx);
Type *type_string(TypeContext *ctx);

/* Create composite types */
Type *type_array(TypeContext *ctx, Type *element);
Type *type_object(TypeContext *ctx, TypeField *fields);
Type *type_tuple(TypeContext *ctx, Type **elements, int count);
Type *type_union(TypeContext *ctx, Type **members, int count);
Type *type_optional(TypeContext *ctx, Type *inner);
Type *type_function(TypeContext *ctx, Type **params, int param_count,
                    Type *ret, bool variadic);

/* Create field for object type */
TypeField *type_field(TypeContext *ctx, const char *name, Type *type, bool optional);

/* Type operations */
bool type_equals(Type *a, Type *b);
bool type_assignable(Type *target, Type *source);  /* Can source be assigned to target? */
Type *type_common(TypeContext *ctx, Type *a, Type *b);  /* Find common type */

/* Type utilities */
bool type_is_numeric(Type *t);
bool type_is_iterable(Type *t);
Type *type_element_of(Type *t);  /* Get element type of array/iterable */
Type *type_field_type(Type *t, const char *field);  /* Get field type from object */

/* Debug: type to string */
const char *type_to_string(TypeContext *ctx, Type *type);

#endif /* MOT_TYPES_H */
