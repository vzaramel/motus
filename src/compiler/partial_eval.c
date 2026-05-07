/*
 * Partial Evaluation implementation
 */

#include "partial_eval.h"
#include "../analyzer/analyzer.h"
#include <stdio.h>
#include <string.h>
#include <ctype.h>

/* Create a new partial evaluator */
PartialEval *pe_new(Arena *arena, struct Analyzer *analyzer) {
    PartialEval *pe = arena_alloc(arena, sizeof(PartialEval));
    pe->arena = arena;
    pe->analyzer = analyzer;
    pe->scope = NULL;
    pe->constants_folded = 0;
    pe->expressions_evaluated = 0;

    /* Start with global scope */
    pe_push_scope(pe);

    return pe;
}

/* Scope management */
void pe_push_scope(PartialEval *pe) {
    PEScope *scope = arena_alloc(pe->arena, sizeof(PEScope));
    scope->bindings = NULL;
    scope->parent = pe->scope;
    pe->scope = scope;
}

void pe_pop_scope(PartialEval *pe) {
    if (pe->scope) {
        pe->scope = pe->scope->parent;
    }
}

/* Binding management */
void pe_define(PartialEval *pe, const char *name, PEValue value, bool is_static) {
    PEBinding *binding = arena_alloc(pe->arena, sizeof(PEBinding));
    size_t len = strlen(name);
    binding->name = arena_alloc(pe->arena, len + 1);
    memcpy(binding->name, name, len + 1);
    binding->value = value;
    binding->is_static = is_static;
    binding->next = pe->scope->bindings;
    pe->scope->bindings = binding;
}

PEBinding *pe_lookup(PartialEval *pe, const char *name) {
    for (PEScope *scope = pe->scope; scope; scope = scope->parent) {
        for (PEBinding *b = scope->bindings; b; b = b->next) {
            if (strcmp(b->name, name) == 0) {
                return b;
            }
        }
    }
    return NULL;
}

/* Value constructors */
PEValue pe_null(void) {
    return (PEValue){ .kind = PE_NULL };
}

PEValue pe_bool(bool value) {
    PEValue v = { .kind = PE_BOOL };
    v.v.boolean = value;
    return v;
}

PEValue pe_int(int64_t value) {
    PEValue v = { .kind = PE_INT };
    v.v.integer = value;
    return v;
}

PEValue pe_number(double value) {
    PEValue v = { .kind = PE_NUMBER };
    v.v.number = value;
    return v;
}

PEValue pe_string(Arena *arena, const char *data, uint32_t length) {
    PEValue v = { .kind = PE_STRING };
    v.v.string.data = arena_alloc(arena, length + 1);
    memcpy(v.v.string.data, data, length);
    v.v.string.data[length] = '\0';
    v.v.string.length = length;
    return v;
}

PEValue pe_array(Arena *arena, PEValue *elements, uint32_t count) {
    PEValue v = { .kind = PE_ARRAY };
    v.v.array.elements = arena_alloc(arena, sizeof(PEValue) * count);
    memcpy(v.v.array.elements, elements, sizeof(PEValue) * count);
    v.v.array.count = count;
    return v;
}

PEValue pe_dynamic(void) {
    return (PEValue){ .kind = PE_DYNAMIC };
}

PEValue pe_unknown(void) {
    return (PEValue){ .kind = PE_UNKNOWN };
}

/* Check if a value is static */
bool pe_is_static(PEValue *value) {
    switch (value->kind) {
        case PE_NULL:
        case PE_BOOL:
        case PE_INT:
        case PE_NUMBER:
        case PE_STRING:
            return true;
        case PE_ARRAY: {
            for (uint32_t i = 0; i < value->v.array.count; i++) {
                if (!pe_is_static(&value->v.array.elements[i])) {
                    return false;
                }
            }
            return true;
        }
        case PE_OBJECT: {
            for (uint32_t i = 0; i < value->v.object.count; i++) {
                if (!pe_is_static(&value->v.object.values[i])) {
                    return false;
                }
            }
            return true;
        }
        case PE_UNKNOWN:
        case PE_DYNAMIC:
            return false;
    }
    return false;
}

/* Convert to number for arithmetic */
static bool to_number(PEValue *v, double *out) {
    switch (v->kind) {
        case PE_INT:
            *out = (double)v->v.integer;
            return true;
        case PE_NUMBER:
            *out = v->v.number;
            return true;
        default:
            return false;
    }
}

/* Convert to integer for integer operations */
static bool to_int(PEValue *v, int64_t *out) {
    switch (v->kind) {
        case PE_INT:
            *out = v->v.integer;
            return true;
        case PE_NUMBER:
            *out = (int64_t)v->v.number;
            return true;
        default:
            return false;
    }
}

/* Convert to boolean */
static bool to_bool(PEValue *v, bool *out) {
    switch (v->kind) {
        case PE_BOOL:
            *out = v->v.boolean;
            return true;
        case PE_NULL:
            *out = false;
            return true;
        case PE_INT:
            *out = v->v.integer != 0;
            return true;
        case PE_NUMBER:
            *out = v->v.number != 0.0;
            return true;
        case PE_STRING:
            *out = v->v.string.length > 0;
            return true;
        default:
            return false;
    }
}

/* Arithmetic operations */
PEValue pe_add(PartialEval *pe, PEValue a, PEValue b) {
    if (!pe_is_static(&a) || !pe_is_static(&b)) {
        return pe_unknown();
    }

    /* String concatenation */
    if (a.kind == PE_STRING && b.kind == PE_STRING) {
        return pe_concat(pe, a, b);
    }

    /* Integer arithmetic (preserve precision) */
    if (a.kind == PE_INT && b.kind == PE_INT) {
        pe->constants_folded++;
        return pe_int(a.v.integer + b.v.integer);
    }

    /* Numeric addition */
    double da, db;
    if (to_number(&a, &da) && to_number(&b, &db)) {
        pe->constants_folded++;
        return pe_number(da + db);
    }

    return pe_unknown();
}

PEValue pe_sub(PartialEval *pe, PEValue a, PEValue b) {
    if (!pe_is_static(&a) || !pe_is_static(&b)) {
        return pe_unknown();
    }

    if (a.kind == PE_INT && b.kind == PE_INT) {
        pe->constants_folded++;
        return pe_int(a.v.integer - b.v.integer);
    }

    double da, db;
    if (to_number(&a, &da) && to_number(&b, &db)) {
        pe->constants_folded++;
        return pe_number(da - db);
    }

    return pe_unknown();
}

PEValue pe_mul(PartialEval *pe, PEValue a, PEValue b) {
    if (!pe_is_static(&a) || !pe_is_static(&b)) {
        return pe_unknown();
    }

    if (a.kind == PE_INT && b.kind == PE_INT) {
        pe->constants_folded++;
        return pe_int(a.v.integer * b.v.integer);
    }

    double da, db;
    if (to_number(&a, &da) && to_number(&b, &db)) {
        pe->constants_folded++;
        return pe_number(da * db);
    }

    return pe_unknown();
}

PEValue pe_div(PartialEval *pe, PEValue a, PEValue b) {
    if (!pe_is_static(&a) || !pe_is_static(&b)) {
        return pe_unknown();
    }

    double da, db;
    if (to_number(&a, &da) && to_number(&b, &db)) {
        if (db == 0.0) {
            return pe_unknown();  /* Division by zero */
        }
        pe->constants_folded++;
        return pe_number(da / db);
    }

    return pe_unknown();
}

PEValue pe_mod(PartialEval *pe, PEValue a, PEValue b) {
    if (!pe_is_static(&a) || !pe_is_static(&b)) {
        return pe_unknown();
    }

    int64_t ia, ib;
    if (to_int(&a, &ia) && to_int(&b, &ib)) {
        if (ib == 0) {
            return pe_unknown();  /* Modulo by zero */
        }
        pe->constants_folded++;
        return pe_int(ia % ib);
    }

    return pe_unknown();
}

PEValue pe_neg(PartialEval *pe, PEValue a) {
    if (!pe_is_static(&a)) {
        return pe_unknown();
    }

    if (a.kind == PE_INT) {
        pe->constants_folded++;
        return pe_int(-a.v.integer);
    }

    if (a.kind == PE_NUMBER) {
        pe->constants_folded++;
        return pe_number(-a.v.number);
    }

    return pe_unknown();
}

/* Comparison operations */
PEValue pe_eq(PartialEval *pe, PEValue a, PEValue b) {
    if (!pe_is_static(&a) || !pe_is_static(&b)) {
        return pe_unknown();
    }

    pe->constants_folded++;

    /* Same kind comparison */
    if (a.kind == b.kind) {
        switch (a.kind) {
            case PE_NULL:
                return pe_bool(true);
            case PE_BOOL:
                return pe_bool(a.v.boolean == b.v.boolean);
            case PE_INT:
                return pe_bool(a.v.integer == b.v.integer);
            case PE_NUMBER:
                return pe_bool(a.v.number == b.v.number);
            case PE_STRING:
                return pe_bool(a.v.string.length == b.v.string.length &&
                               memcmp(a.v.string.data, b.v.string.data,
                                      a.v.string.length) == 0);
            default:
                return pe_unknown();
        }
    }

    /* Cross-type numeric comparison */
    double da, db;
    if (to_number(&a, &da) && to_number(&b, &db)) {
        return pe_bool(da == db);
    }

    return pe_bool(false);
}

PEValue pe_neq(PartialEval *pe, PEValue a, PEValue b) {
    PEValue eq = pe_eq(pe, a, b);
    if (eq.kind == PE_BOOL) {
        return pe_bool(!eq.v.boolean);
    }
    return pe_unknown();
}

PEValue pe_lt(PartialEval *pe, PEValue a, PEValue b) {
    if (!pe_is_static(&a) || !pe_is_static(&b)) {
        return pe_unknown();
    }

    double da, db;
    if (to_number(&a, &da) && to_number(&b, &db)) {
        pe->constants_folded++;
        return pe_bool(da < db);
    }

    /* String comparison */
    if (a.kind == PE_STRING && b.kind == PE_STRING) {
        pe->constants_folded++;
        return pe_bool(strcmp(a.v.string.data, b.v.string.data) < 0);
    }

    return pe_unknown();
}

PEValue pe_lte(PartialEval *pe, PEValue a, PEValue b) {
    if (!pe_is_static(&a) || !pe_is_static(&b)) {
        return pe_unknown();
    }

    double da, db;
    if (to_number(&a, &da) && to_number(&b, &db)) {
        pe->constants_folded++;
        return pe_bool(da <= db);
    }

    if (a.kind == PE_STRING && b.kind == PE_STRING) {
        pe->constants_folded++;
        return pe_bool(strcmp(a.v.string.data, b.v.string.data) <= 0);
    }

    return pe_unknown();
}

PEValue pe_gt(PartialEval *pe, PEValue a, PEValue b) {
    if (!pe_is_static(&a) || !pe_is_static(&b)) {
        return pe_unknown();
    }

    double da, db;
    if (to_number(&a, &da) && to_number(&b, &db)) {
        pe->constants_folded++;
        return pe_bool(da > db);
    }

    if (a.kind == PE_STRING && b.kind == PE_STRING) {
        pe->constants_folded++;
        return pe_bool(strcmp(a.v.string.data, b.v.string.data) > 0);
    }

    return pe_unknown();
}

PEValue pe_gte(PartialEval *pe, PEValue a, PEValue b) {
    if (!pe_is_static(&a) || !pe_is_static(&b)) {
        return pe_unknown();
    }

    double da, db;
    if (to_number(&a, &da) && to_number(&b, &db)) {
        pe->constants_folded++;
        return pe_bool(da >= db);
    }

    if (a.kind == PE_STRING && b.kind == PE_STRING) {
        pe->constants_folded++;
        return pe_bool(strcmp(a.v.string.data, b.v.string.data) >= 0);
    }

    return pe_unknown();
}

/* Logical operations */
PEValue pe_and(PartialEval *pe, PEValue a, PEValue b) {
    bool ba;
    if (to_bool(&a, &ba)) {
        if (!ba) {
            pe->constants_folded++;
            return pe_bool(false);  /* Short-circuit */
        }
        /* a is true, result depends on b */
        bool bb;
        if (to_bool(&b, &bb)) {
            pe->constants_folded++;
            return pe_bool(bb);
        }
    }
    return pe_unknown();
}

PEValue pe_or(PartialEval *pe, PEValue a, PEValue b) {
    bool ba;
    if (to_bool(&a, &ba)) {
        if (ba) {
            pe->constants_folded++;
            return pe_bool(true);  /* Short-circuit */
        }
        /* a is false, result depends on b */
        bool bb;
        if (to_bool(&b, &bb)) {
            pe->constants_folded++;
            return pe_bool(bb);
        }
    }
    return pe_unknown();
}

PEValue pe_not(PartialEval *pe, PEValue a) {
    bool ba;
    if (to_bool(&a, &ba)) {
        pe->constants_folded++;
        return pe_bool(!ba);
    }
    return pe_unknown();
}

/* String operations */
PEValue pe_concat(PartialEval *pe, PEValue a, PEValue b) {
    if (a.kind != PE_STRING || b.kind != PE_STRING) {
        return pe_unknown();
    }

    uint32_t new_len = a.v.string.length + b.v.string.length;
    char *data = arena_alloc(pe->arena, new_len + 1);
    memcpy(data, a.v.string.data, a.v.string.length);
    memcpy(data + a.v.string.length, b.v.string.data, b.v.string.length);
    data[new_len] = '\0';

    pe->constants_folded++;
    return pe_string(pe->arena, data, new_len);
}

PEValue pe_uppercase(PartialEval *pe, PEValue a) {
    if (a.kind != PE_STRING) {
        return pe_unknown();
    }

    char *data = arena_alloc(pe->arena, a.v.string.length + 1);
    for (uint32_t i = 0; i < a.v.string.length; i++) {
        data[i] = (char)toupper((unsigned char)a.v.string.data[i]);
    }
    data[a.v.string.length] = '\0';

    pe->constants_folded++;
    return pe_string(pe->arena, data, a.v.string.length);
}

PEValue pe_lowercase(PartialEval *pe, PEValue a) {
    if (a.kind != PE_STRING) {
        return pe_unknown();
    }

    char *data = arena_alloc(pe->arena, a.v.string.length + 1);
    for (uint32_t i = 0; i < a.v.string.length; i++) {
        data[i] = (char)tolower((unsigned char)a.v.string.data[i]);
    }
    data[a.v.string.length] = '\0';

    pe->constants_folded++;
    return pe_string(pe->arena, data, a.v.string.length);
}

PEValue pe_trim(PartialEval *pe, PEValue a) {
    if (a.kind != PE_STRING) {
        return pe_unknown();
    }

    const char *start = a.v.string.data;
    const char *end = a.v.string.data + a.v.string.length;

    /* Trim leading whitespace */
    while (start < end && isspace((unsigned char)*start)) {
        start++;
    }

    /* Trim trailing whitespace */
    while (end > start && isspace((unsigned char)*(end - 1))) {
        end--;
    }

    uint32_t new_len = (uint32_t)(end - start);
    pe->constants_folded++;
    return pe_string(pe->arena, start, new_len);
}

/* Evaluate literal node */
static PEValue eval_literal(PartialEval *pe, AstNode *node) {
    (void)pe;  /* Unused for now */

    switch (node->data.literal.lit_type) {
        case LIT_NULL:
            return pe_null();
        case LIT_BOOL:
            return pe_bool(node->data.literal.v.boolean);
        case LIT_INT:
            return pe_int(node->data.literal.v.integer);
        case LIT_NUMBER:
            return pe_number(node->data.literal.v.number);
        case LIT_STRING:
            return pe_string(pe->arena,
                             node->data.literal.v.string.value,
                             node->data.literal.v.string.length);
    }
    return pe_unknown();
}

/* Evaluate identifier */
static PEValue eval_ident(PartialEval *pe, AstNode *node) {
    const char *name = node->data.ident.name;
    PEBinding *binding = pe_lookup(pe, name);

    if (binding && binding->is_static) {
        return binding->value;
    }

    /* Unknown or dynamic binding */
    return pe_unknown();
}

/* Evaluate binary expression */
static PEValue eval_binary(PartialEval *pe, AstNode *node) {
    PEValue left = pe_eval(pe, node->data.binary.left);
    PEValue right = pe_eval(pe, node->data.binary.right);

    switch (node->data.binary.op) {
        case OP_ADD: return pe_add(pe, left, right);
        case OP_SUB: return pe_sub(pe, left, right);
        case OP_MUL: return pe_mul(pe, left, right);
        case OP_DIV: return pe_div(pe, left, right);
        case OP_MOD: return pe_mod(pe, left, right);
        case OP_LT:  return pe_lt(pe, left, right);
        case OP_LTE: return pe_lte(pe, left, right);
        case OP_GT:  return pe_gt(pe, left, right);
        case OP_GTE: return pe_gte(pe, left, right);
        case OP_EQ:  return pe_eq(pe, left, right);
        case OP_NEQ: return pe_neq(pe, left, right);
        case OP_AND: return pe_and(pe, left, right);
        case OP_OR:  return pe_or(pe, left, right);
        default:
            return pe_unknown();
    }
}

/* Evaluate unary expression */
static PEValue eval_unary(PartialEval *pe, AstNode *node) {
    PEValue operand = pe_eval(pe, node->data.unary.operand);

    switch (node->data.unary.op) {
        case OP_NEG: return pe_neg(pe, operand);
        case OP_NOT: return pe_not(pe, operand);
        default:
            return pe_unknown();
    }
}

/* Evaluate ternary expression */
static PEValue eval_ternary(PartialEval *pe, AstNode *node) {
    PEValue cond = pe_eval(pe, node->data.ternary.condition);
    bool cond_bool = false;
    if (!to_bool(&cond, &cond_bool)) {
        return pe_unknown();
    }

    if (cond_bool) {
        return pe_eval(pe, node->data.ternary.then_expr);
    }
    return pe_eval(pe, node->data.ternary.else_expr);
}

/* Evaluate array literal */
static PEValue eval_array(PartialEval *pe, AstNode *node) {
    /* Count elements */
    uint32_t count = 0;
    for (AstNode *elem = node->data.array.elements; elem; elem = elem->next) {
        count++;
    }

    if (count == 0) {
        return pe_array(pe->arena, NULL, 0);
    }

    /* Evaluate elements */
    PEValue *elements = arena_alloc(pe->arena, sizeof(PEValue) * count);
    uint32_t i = 0;
    bool all_static = true;

    for (AstNode *elem = node->data.array.elements; elem; elem = elem->next) {
        elements[i] = pe_eval(pe, elem);
        if (!pe_is_static(&elements[i])) {
            all_static = false;
        }
        i++;
    }

    if (!all_static) {
        return pe_unknown();
    }

    return pe_array(pe->arena, elements, count);
}

/* Evaluate pipe expression */
static PEValue eval_pipe(PartialEval *pe, AstNode *node) {
    PEValue value = pe_eval(pe, node->data.pipe.input);

    /* Apply each stage */
    for (AstNode *stage = node->data.pipe.stages; stage; stage = stage->next) {
        if (stage->type == NODE_IDENT) {
            const char *fn = stage->data.ident.name;

            /* Apply known builtins */
            if (strcmp(fn, "uppercase") == 0) {
                value = pe_uppercase(pe, value);
            } else if (strcmp(fn, "lowercase") == 0) {
                value = pe_lowercase(pe, value);
            } else if (strcmp(fn, "trim") == 0) {
                value = pe_trim(pe, value);
            } else {
                /* Unknown function, can't evaluate statically */
                return pe_unknown();
            }
        } else if (stage->type == NODE_CALL) {
            const char *fn = stage->data.call.name;

            if (strcmp(fn, "uppercase") == 0) {
                value = pe_uppercase(pe, value);
            } else if (strcmp(fn, "lowercase") == 0) {
                value = pe_lowercase(pe, value);
            } else if (strcmp(fn, "trim") == 0) {
                value = pe_trim(pe, value);
            } else {
                return pe_unknown();
            }
        } else {
            return pe_unknown();
        }

        if (!pe_is_static(&value)) {
            return pe_unknown();
        }
    }

    return value;
}

/* Evaluate call expression for selected pure builtins */
static PEValue eval_call(PartialEval *pe, AstNode *node) {
    const char *fn = node->data.call.name;

    if (!node->data.call.args || node->data.call.args->next) {
        return pe_unknown();
    }

    PEValue arg = pe_eval(pe, node->data.call.args);

    if (strcmp(fn, "uppercase") == 0) return pe_uppercase(pe, arg);
    if (strcmp(fn, "lowercase") == 0) return pe_lowercase(pe, arg);
    if (strcmp(fn, "trim") == 0) return pe_trim(pe, arg);

    return pe_unknown();
}

/* Main evaluation function */
PEValue pe_eval(PartialEval *pe, AstNode *expr) {
    if (!expr) {
        return pe_null();
    }

    pe->expressions_evaluated++;

    switch (expr->type) {
        case NODE_LITERAL:
            return eval_literal(pe, expr);

        case NODE_IDENT:
            return eval_ident(pe, expr);

        case NODE_BINARY:
            return eval_binary(pe, expr);

        case NODE_UNARY:
            return eval_unary(pe, expr);

        case NODE_TERNARY:
            return eval_ternary(pe, expr);

        case NODE_ARRAY:
            return eval_array(pe, expr);

        case NODE_PIPE:
            return eval_pipe(pe, expr);

        case NODE_CALL:
            return eval_call(pe, expr);

        /* These require runtime evaluation */
        case NODE_MEMBER:
        case NODE_INDEX:
        case NODE_SQL:
            return pe_unknown();

        default:
            return pe_unknown();
    }
}

/* Check if expression can be evaluated statically */
bool pe_expr_is_static(PartialEval *pe, AstNode *expr) {
    PEValue val = pe_eval(pe, expr);
    return pe_is_static(&val);
}

/* Debug printing */
void pe_value_print(PEValue *value) {
    switch (value->kind) {
        case PE_UNKNOWN:
            printf("<unknown>");
            break;
        case PE_DYNAMIC:
            printf("<dynamic>");
            break;
        case PE_NULL:
            printf("null");
            break;
        case PE_BOOL:
            printf("%s", value->v.boolean ? "true" : "false");
            break;
        case PE_INT:
            printf("%lld", (long long)value->v.integer);
            break;
        case PE_NUMBER:
            printf("%g", value->v.number);
            break;
        case PE_STRING:
            printf("\"%s\"", value->v.string.data);
            break;
        case PE_ARRAY:
            printf("[");
            for (uint32_t i = 0; i < value->v.array.count; i++) {
                if (i > 0) printf(", ");
                pe_value_print(&value->v.array.elements[i]);
            }
            printf("]");
            break;
        case PE_OBJECT:
            printf("{");
            for (uint32_t i = 0; i < value->v.object.count; i++) {
                if (i > 0) printf(", ");
                printf("%s: ", value->v.object.keys[i]);
                pe_value_print(&value->v.object.values[i]);
            }
            printf("}");
            break;
    }
}
