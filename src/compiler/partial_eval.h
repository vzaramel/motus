/*
 * Partial Evaluation for Motus
 *
 * Resolves static expressions at compile time, leaving "holes"
 * for dynamic data that must be computed at runtime.
 */

#ifndef MOT_PARTIAL_EVAL_H
#define MOT_PARTIAL_EVAL_H

#include <stdint.h>
#include <stdbool.h>
#include "../util/arena.h"
#include "../parser/ast.h"

/* Forward declarations */
struct Analyzer;

/* Partial evaluation value types */
typedef enum {
    PE_UNKNOWN,      /* Cannot be determined statically */
    PE_NULL,
    PE_BOOL,
    PE_INT,
    PE_NUMBER,
    PE_STRING,
    PE_ARRAY,
    PE_OBJECT,
    PE_DYNAMIC,      /* Known to be dynamic (runtime data) */
} PEValueKind;

/* Compile-time value */
typedef struct PEValue {
    PEValueKind kind;
    union {
        bool boolean;
        int64_t integer;
        double number;
        struct {
            char *data;
            uint32_t length;
        } string;
        struct {
            struct PEValue *elements;
            uint32_t count;
        } array;
        struct {
            char **keys;
            struct PEValue *values;
            uint32_t count;
        } object;
    } v;
} PEValue;

/* Binding entry for partial evaluation context */
typedef struct PEBinding {
    char *name;
    PEValue value;
    bool is_static;          /* True if value is known at compile time */
    struct PEBinding *next;
} PEBinding;

/* Partial evaluation scope */
typedef struct PEScope {
    PEBinding *bindings;
    struct PEScope *parent;
} PEScope;

/* Partial evaluator context */
typedef struct {
    Arena *arena;
    struct Analyzer *analyzer;   /* For type info */
    PEScope *scope;

    /* Statistics */
    uint32_t constants_folded;
    uint32_t expressions_evaluated;
} PartialEval;

/* Create a new partial evaluator */
PartialEval *pe_new(Arena *arena, struct Analyzer *analyzer);

/* Scope management */
void pe_push_scope(PartialEval *pe);
void pe_pop_scope(PartialEval *pe);

/* Binding management */
void pe_define(PartialEval *pe, const char *name, PEValue value, bool is_static);
PEBinding *pe_lookup(PartialEval *pe, const char *name);

/* Evaluate an expression at compile time */
PEValue pe_eval(PartialEval *pe, AstNode *expr);

/* Check if a value is static (known at compile time) */
bool pe_is_static(PEValue *value);

/* Check if an expression can be evaluated statically */
bool pe_expr_is_static(PartialEval *pe, AstNode *expr);

/* Value constructors */
PEValue pe_null(void);
PEValue pe_bool(bool value);
PEValue pe_int(int64_t value);
PEValue pe_number(double value);
PEValue pe_string(Arena *arena, const char *data, uint32_t length);
PEValue pe_array(Arena *arena, PEValue *elements, uint32_t count);
PEValue pe_dynamic(void);
PEValue pe_unknown(void);

/* Value operations */
PEValue pe_add(PartialEval *pe, PEValue a, PEValue b);
PEValue pe_sub(PartialEval *pe, PEValue a, PEValue b);
PEValue pe_mul(PartialEval *pe, PEValue a, PEValue b);
PEValue pe_div(PartialEval *pe, PEValue a, PEValue b);
PEValue pe_mod(PartialEval *pe, PEValue a, PEValue b);
PEValue pe_neg(PartialEval *pe, PEValue a);

PEValue pe_eq(PartialEval *pe, PEValue a, PEValue b);
PEValue pe_neq(PartialEval *pe, PEValue a, PEValue b);
PEValue pe_lt(PartialEval *pe, PEValue a, PEValue b);
PEValue pe_lte(PartialEval *pe, PEValue a, PEValue b);
PEValue pe_gt(PartialEval *pe, PEValue a, PEValue b);
PEValue pe_gte(PartialEval *pe, PEValue a, PEValue b);

PEValue pe_and(PartialEval *pe, PEValue a, PEValue b);
PEValue pe_or(PartialEval *pe, PEValue a, PEValue b);
PEValue pe_not(PartialEval *pe, PEValue a);

/* String operations */
PEValue pe_concat(PartialEval *pe, PEValue a, PEValue b);
PEValue pe_uppercase(PartialEval *pe, PEValue a);
PEValue pe_lowercase(PartialEval *pe, PEValue a);
PEValue pe_trim(PartialEval *pe, PEValue a);

/* Convert PEValue to string for debugging */
void pe_value_print(PEValue *value);

#endif /* MOT_PARTIAL_EVAL_H */
