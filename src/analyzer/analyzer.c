/*
 * Semantic analyzer implementation
 */

#include "analyzer.h"
#include <stdio.h>
#include <stdarg.h>
#include <string.h>

/* Forward declarations */
static void analyze_node(Analyzer *a, AstNode *node);
static ExprResult analyze_expr(Analyzer *a, AstNode *expr);

Analyzer *analyzer_new(Arena *arena) {
    Analyzer *a = arena_alloc(arena, sizeof(Analyzer));
    a->arena = arena;
    a->types = type_context_new(arena);
    a->deps = dep_context_new(arena);
    a->scope = NULL;
    a->global_scope = NULL;
    a->errors = NULL;
    a->error_tail = &a->errors;
    a->error_count = 0;
    a->strict_types = false;
    a->track_deps = true;

    return a;
}

void analyzer_error(Analyzer *a, int line, int col, const char *fmt, ...) {
    /* Format message */
    char buf[512];
    va_list args;
    va_start(args, fmt);
    vsnprintf(buf, sizeof(buf), fmt, args);
    va_end(args);

    /* Create error */
    AnalysisError *err = arena_alloc(a->arena, sizeof(AnalysisError));
    err->message = arena_strdup(a->arena, buf);
    err->line = line;
    err->col = col;
    err->next = NULL;

    /* Append to list */
    *a->error_tail = err;
    a->error_tail = &err->next;
    a->error_count++;
}

AnalysisError *analyzer_errors(Analyzer *a) {
    return a->errors;
}

int analyzer_error_count(Analyzer *a) {
    return a->error_count;
}

bool analyzer_ok(Analyzer *a) {
    return a->error_count == 0;
}

void analyzer_push_scope(Analyzer *a, ScopeKind kind) {
    a->scope = scope_new(a->arena, kind, a->scope);
}

void analyzer_pop_scope(Analyzer *a) {
    if (a->scope) {
        a->scope = a->scope->parent;
    }
}

Type *analyzer_resolve_type_name(Analyzer *a, const char *name) {
    if (!name) return type_unknown(a->types);

    if (strcmp(name, "string") == 0) return type_string(a->types);
    if (strcmp(name, "number") == 0) return type_number(a->types);
    if (strcmp(name, "int") == 0) return type_int(a->types);
    if (strcmp(name, "bool") == 0) return type_bool(a->types);
    if (strcmp(name, "any") == 0) return type_any(a->types);
    if (strcmp(name, "void") == 0) return type_void(a->types);

    /* TODO: Look up user-defined types */
    return type_unknown(a->types);
}

/* Register built-in functions and values */
void analyzer_register_builtins(Analyzer *a) {
    Scope *scope = a->global_scope;

    /* String functions */
    Symbol *sym;

    sym = scope_define(scope, "uppercase", SYM_BUILTIN, NULL, 0, 0);
    symbol_set_type(sym, type_function(a->types,
        (Type*[]){type_string(a->types)}, 1, type_string(a->types), false));

    sym = scope_define(scope, "lowercase", SYM_BUILTIN, NULL, 0, 0);
    symbol_set_type(sym, type_function(a->types,
        (Type*[]){type_string(a->types)}, 1, type_string(a->types), false));

    sym = scope_define(scope, "trim", SYM_BUILTIN, NULL, 0, 0);
    symbol_set_type(sym, type_function(a->types,
        (Type*[]){type_string(a->types)}, 1, type_string(a->types), false));

    sym = scope_define(scope, "length", SYM_BUILTIN, NULL, 0, 0);
    symbol_set_type(sym, type_function(a->types,
        (Type*[]){type_any(a->types)}, 1, type_int(a->types), false));

    sym = scope_define(scope, "concat", SYM_BUILTIN, NULL, 0, 0);
    symbol_set_type(sym, type_function(a->types,
        (Type*[]){type_string(a->types), type_string(a->types)}, 2,
        type_string(a->types), true));

    /* Numeric functions */
    sym = scope_define(scope, "abs", SYM_BUILTIN, NULL, 0, 0);
    symbol_set_type(sym, type_function(a->types,
        (Type*[]){type_number(a->types)}, 1, type_number(a->types), false));

    sym = scope_define(scope, "round", SYM_BUILTIN, NULL, 0, 0);
    symbol_set_type(sym, type_function(a->types,
        (Type*[]){type_number(a->types)}, 1, type_int(a->types), false));

    sym = scope_define(scope, "floor", SYM_BUILTIN, NULL, 0, 0);
    symbol_set_type(sym, type_function(a->types,
        (Type*[]){type_number(a->types)}, 1, type_int(a->types), false));

    sym = scope_define(scope, "ceil", SYM_BUILTIN, NULL, 0, 0);
    symbol_set_type(sym, type_function(a->types,
        (Type*[]){type_number(a->types)}, 1, type_int(a->types), false));

    /* Array functions */
    sym = scope_define(scope, "first", SYM_BUILTIN, NULL, 0, 0);
    symbol_set_type(sym, type_function(a->types,
        (Type*[]){type_any(a->types)}, 1, type_any(a->types), false));

    sym = scope_define(scope, "last", SYM_BUILTIN, NULL, 0, 0);
    symbol_set_type(sym, type_function(a->types,
        (Type*[]){type_any(a->types)}, 1, type_any(a->types), false));

    sym = scope_define(scope, "reverse", SYM_BUILTIN, NULL, 0, 0);
    symbol_set_type(sym, type_function(a->types,
        (Type*[]){type_any(a->types)}, 1, type_any(a->types), false));

    sym = scope_define(scope, "sort", SYM_BUILTIN, NULL, 0, 0);
    symbol_set_type(sym, type_function(a->types,
        (Type*[]){type_any(a->types)}, 1, type_any(a->types), false));

    /* Date/time functions */
    sym = scope_define(scope, "now", SYM_BUILTIN, NULL, 0, 0);
    symbol_set_type(sym, type_function(a->types, NULL, 0, type_string(a->types), false));

    sym = scope_define(scope, "formatDate", SYM_BUILTIN, NULL, 0, 0);
    symbol_set_type(sym, type_function(a->types,
        (Type*[]){type_string(a->types), type_string(a->types)}, 2,
        type_string(a->types), false));

    /* Type checking */
    sym = scope_define(scope, "typeof", SYM_BUILTIN, NULL, 0, 0);
    symbol_set_type(sym, type_function(a->types,
        (Type*[]){type_any(a->types)}, 1, type_string(a->types), false));

    sym = scope_define(scope, "defined", SYM_BUILTIN, NULL, 0, 0);
    symbol_set_type(sym, type_function(a->types,
        (Type*[]){type_any(a->types)}, 1, type_bool(a->types), false));

    /* JSON */
    sym = scope_define(scope, "json", SYM_BUILTIN, NULL, 0, 0);
    symbol_set_type(sym, type_function(a->types,
        (Type*[]){type_any(a->types)}, 1, type_string(a->types), false));

    /* HTML escaping */
    sym = scope_define(scope, "escape", SYM_BUILTIN, NULL, 0, 0);
    symbol_set_type(sym, type_function(a->types,
        (Type*[]){type_string(a->types)}, 1, type_string(a->types), false));

    sym = scope_define(scope, "raw", SYM_BUILTIN, NULL, 0, 0);
    symbol_set_type(sym, type_function(a->types,
        (Type*[]){type_string(a->types)}, 1, type_string(a->types), false));
}

/* Analyze a literal node */
static ExprResult analyze_literal(Analyzer *a, AstNode *node) {
    ExprResult result = {
        .type = type_unknown(a->types),
        .deps = dep_set_new(a->arena),
        .is_static = true,
        .is_constant = true
    };

    switch (node->data.literal.lit_type) {
        case LIT_NUMBER:
            result.type = type_number(a->types);
            break;
        case LIT_INT:
            result.type = type_int(a->types);
            break;
        case LIT_STRING:
            result.type = type_string(a->types);
            break;
        case LIT_BOOL:
            result.type = type_bool(a->types);
            break;
        case LIT_NULL:
            result.type = type_null(a->types);
            break;
    }

    return result;
}

/* Analyze an identifier reference */
static ExprResult analyze_ident(Analyzer *a, AstNode *node) {
    ExprResult result = {
        .type = type_unknown(a->types),
        .deps = dep_set_new(a->arena),
        .is_static = false,
        .is_constant = false
    };

    const char *name = node->data.ident.name;
    Symbol *sym = scope_lookup(a->scope, name);

    if (!sym) {
        analyzer_error(a, node->line, node->column,
                       "Undefined variable '%s'", name);
        return result;
    }

    /* Mark as used */
    symbol_set_flags(sym, SYM_FLAG_USED);

    result.type = sym->type ? sym->type : type_unknown(a->types);

    /* Track dependency */
    if (a->track_deps && sym->kind != SYM_BUILTIN) {
        DepPath *path = dep_path_var(a->deps, name);
        dep_set_add(result.deps, path);
        dep_record(a->deps, path);
    }

    /* Static if it's a static binding */
    result.is_static = !(sym->flags & SYM_FLAG_DYNAMIC);

    return result;
}

/* Analyze member access (a.b) */
static ExprResult analyze_member(Analyzer *a, AstNode *node) {
    ExprResult base = analyze_expr(a, node->data.member.object);

    ExprResult result = {
        .type = type_unknown(a->types),
        .deps = dep_set_new(a->arena),
        .is_static = base.is_static,
        .is_constant = false
    };

    const char *field = node->data.member.member;

    /* Get field type if base is an object */
    if (base.type->kind == TYPE_OBJECT) {
        Type *field_type = type_field_type(base.type, field);
        if (field_type) {
            result.type = field_type;
        } else {
            analyzer_error(a, node->line, node->column,
                           "Unknown field '%s'", field);
        }
    } else if (base.type->kind == TYPE_ANY || base.type->kind == TYPE_UNKNOWN) {
        result.type = type_any(a->types);
    }

    /* Extend dependencies */
    if (a->track_deps) {
        for (DepPath *p = base.deps->paths; p; p = p->next) {
            DepPath *extended = dep_path_field(a->deps, p, field);
            dep_set_add(result.deps, extended);
            dep_record(a->deps, extended);
        }
    }

    return result;
}

/* Analyze index access (a[i]) */
static ExprResult analyze_index(Analyzer *a, AstNode *node) {
    ExprResult base = analyze_expr(a, node->data.index.object);
    ExprResult idx = analyze_expr(a, node->data.index.index);

    ExprResult result = {
        .type = type_unknown(a->types),
        .deps = dep_set_union(a->deps, base.deps, idx.deps),
        .is_static = base.is_static && idx.is_static,
        .is_constant = false
    };

    /* Get element type if base is an array */
    if (base.type->kind == TYPE_ARRAY) {
        result.type = base.type->data.array.element;
    } else if (base.type->kind == TYPE_STRING) {
        result.type = type_string(a->types);  /* String indexing returns string */
    } else if (base.type->kind == TYPE_ANY || base.type->kind == TYPE_UNKNOWN) {
        result.type = type_any(a->types);
    }

    /* Extend dependencies with index */
    if (a->track_deps) {
        int index_val = -1;
        if (idx.is_constant && idx.type->kind == TYPE_INT) {
            /* Could extract constant index value here */
        }

        for (DepPath *p = base.deps->paths; p; p = p->next) {
            DepPath *extended = (index_val >= 0)
                ? dep_path_index(a->deps, p, index_val)
                : dep_path_dynamic_index(a->deps, p);
            dep_set_add(result.deps, extended);
            dep_record(a->deps, extended);
        }
    }

    return result;
}

/* Analyze binary operation */
static ExprResult analyze_binary(Analyzer *a, AstNode *node) {
    ExprResult left = analyze_expr(a, node->data.binary.left);
    ExprResult right = analyze_expr(a, node->data.binary.right);

    ExprResult result = {
        .type = type_unknown(a->types),
        .deps = dep_set_union(a->deps, left.deps, right.deps),
        .is_static = left.is_static && right.is_static,
        .is_constant = left.is_constant && right.is_constant
    };

    OpType op = node->data.binary.op;

    switch (op) {
        case OP_ADD:
        case OP_SUB:
        case OP_MUL:
        case OP_DIV:
        case OP_MOD:
            /* Arithmetic - result is numeric */
            if (type_is_numeric(left.type) && type_is_numeric(right.type)) {
                result.type = type_common(a->types, left.type, right.type);
            } else if (op == OP_ADD &&
                       (left.type->kind == TYPE_STRING || right.type->kind == TYPE_STRING)) {
                result.type = type_string(a->types);  /* String concatenation */
            } else {
                analyzer_error(a, node->line, node->column,
                               "Invalid operand types for arithmetic");
            }
            break;

        case OP_EQ:
        case OP_NEQ:
        case OP_LT:
        case OP_LTE:
        case OP_GT:
        case OP_GTE:
            /* Comparison - result is bool */
            result.type = type_bool(a->types);
            break;

        case OP_AND:
        case OP_OR:
            /* Logical - result is bool */
            result.type = type_bool(a->types);
            if (left.type->kind != TYPE_BOOL && left.type->kind != TYPE_ANY) {
                analyzer_error(a, node->line, node->column,
                               "Expected boolean for logical operator");
            }
            break;

        default:
            break;
    }

    return result;
}

/* Analyze function call */
static ExprResult analyze_call(Analyzer *a, AstNode *node) {
    ExprResult result = {
        .type = type_unknown(a->types),
        .deps = dep_set_new(a->arena),
        .is_static = true,
        .is_constant = false
    };

    const char *name = node->data.call.name;
    Symbol *fn = scope_lookup(a->scope, name);

    if (!fn) {
        analyzer_error(a, node->line, node->column,
                       "Unknown function '%s'", name);
        return result;
    }

    if (fn->type && fn->type->kind == TYPE_FUNCTION) {
        result.type = fn->type->data.func.ret;
    }

    /* Analyze arguments */
    for (AstNode *arg = node->data.call.args; arg; arg = arg->next) {
        ExprResult arg_result = analyze_expr(a, arg);
        result.deps = dep_set_union(a->deps, result.deps, arg_result.deps);
        result.is_static = result.is_static && arg_result.is_static;
    }

    return result;
}

/* Analyze pipe expression */
static ExprResult analyze_pipe(Analyzer *a, AstNode *node) {
    ExprResult input = analyze_expr(a, node->data.pipe.input);

    ExprResult result = {
        .type = input.type,
        .deps = input.deps,
        .is_static = input.is_static,
        .is_constant = false
    };

    /* Apply each stage */
    for (AstNode *stage = node->data.pipe.stages; stage; stage = stage->next) {
        if (stage->type == NODE_CALL) {
            const char *fn_name = stage->data.call.name;
            Symbol *fn = scope_lookup(a->scope, fn_name);

            if (!fn) {
                analyzer_error(a, stage->line, stage->column,
                               "Unknown pipe function '%s'", fn_name);
                continue;
            }

            if (fn->type && fn->type->kind == TYPE_FUNCTION) {
                result.type = fn->type->data.func.ret;
            }
        }
    }

    return result;
}

/* Analyze unary operation */
static ExprResult analyze_unary(Analyzer *a, AstNode *node) {
    ExprResult operand = analyze_expr(a, node->data.unary.operand);

    ExprResult result = {
        .type = type_unknown(a->types),
        .deps = operand.deps,
        .is_static = operand.is_static,
        .is_constant = operand.is_constant
    };

    if (node->data.unary.op == OP_NEG) {
        if (type_is_numeric(operand.type) ||
            operand.type->kind == TYPE_ANY ||
            operand.type->kind == TYPE_UNKNOWN) {
            result.type = operand.type;
        } else {
            analyzer_error(a, node->line, node->column,
                           "Unary '-' expects numeric operand");
        }
    } else if (node->data.unary.op == OP_NOT) {
        result.type = type_bool(a->types);
    }

    return result;
}

/* Analyze ternary expression: if cond then a else b */
static ExprResult analyze_ternary(Analyzer *a, AstNode *node) {
    ExprResult cond = analyze_expr(a, node->data.ternary.condition);
    ExprResult then_expr = analyze_expr(a, node->data.ternary.then_expr);
    ExprResult else_expr = analyze_expr(a, node->data.ternary.else_expr);

    ExprResult result = {
        .type = type_common(a->types, then_expr.type, else_expr.type),
        .deps = dep_set_union(a->deps, dep_set_union(a->deps, cond.deps, then_expr.deps), else_expr.deps),
        .is_static = cond.is_static && then_expr.is_static && else_expr.is_static,
        .is_constant = cond.is_constant && then_expr.is_constant && else_expr.is_constant
    };

    if (cond.type->kind != TYPE_BOOL &&
        cond.type->kind != TYPE_ANY &&
        cond.type->kind != TYPE_UNKNOWN) {
        analyzer_error(a, node->line, node->column,
                       "Ternary condition must be boolean");
    }

    return result;
}

/* Analyze expression dispatch */
static ExprResult analyze_expr(Analyzer *a, AstNode *expr) {
    if (!expr) {
        return (ExprResult){
            .type = type_void(a->types),
            .deps = dep_set_new(a->arena),
            .is_static = true,
            .is_constant = false
        };
    }

    ExprResult result;

    switch (expr->type) {
        case NODE_LITERAL:
            result = analyze_literal(a, expr);
            break;
        case NODE_IDENT:
            result = analyze_ident(a, expr);
            break;
        case NODE_MEMBER:
            result = analyze_member(a, expr);
            break;
        case NODE_INDEX:
            result = analyze_index(a, expr);
            break;
        case NODE_BINARY:
            result = analyze_binary(a, expr);
            break;
        case NODE_UNARY:
            result = analyze_unary(a, expr);
            break;
        case NODE_CALL:
            result = analyze_call(a, expr);
            break;
        case NODE_PIPE:
            result = analyze_pipe(a, expr);
            break;
        case NODE_TERNARY:
            result = analyze_ternary(a, expr);
            break;
        default:
            result = (ExprResult){
                .type = type_any(a->types),
                .deps = dep_set_new(a->arena),
                .is_static = false,
                .is_constant = false
            };
            break;
    }

    /* Store analysis results on AST node for use by compiler */
    expr->resolved_type = result.type;
    expr->deps = result.deps;

    return result;
}

/* Analyze let binding */
static void analyze_let(Analyzer *a, AstNode *node) {
    const char *name = node->data.binding.name;

    /* Analyze value expression in current scope (before pushing let scope) */
    Type *val_type = type_unknown(a->types);

    if (node->data.binding.value) {
        ExprResult val = analyze_expr(a, node->data.binding.value);
        val_type = val.type;
    } else if (node->data.binding.sql) {
        /* SQL query - type is array of row objects */
        val_type = type_array(a->types, type_any(a->types));
    }

    /* Let creates a new scope - the binding is visible only inside this scope */
    analyzer_push_scope(a, SCOPE_LET);

    /* Check for redefinition in this scope */
    if (scope_is_defined_local(a->scope, name)) {
        analyzer_error(a, node->line, node->column,
                       "Variable '%s' already defined in this scope", name);
    }

    /* Define symbol in the let scope (visible to children only) */
    Symbol *sym = scope_define(a->scope, name, SYM_LET, node, node->line, node->column);
    symbol_set_type(sym, val_type);

    if (node->data.binding.dynamic) {
        symbol_set_flags(sym, SYM_FLAG_DYNAMIC);
    }
    if (node->data.binding.single) {
        symbol_set_flags(sym, SYM_FLAG_SINGLE);
    }

    /* Analyze children in this scope */
    for (AstNode *child = node->data.binding.body; child; child = child->next) {
        analyze_node(a, child);
    }

    analyzer_pop_scope(a);
}

/* Analyze var binding */
static void analyze_var(Analyzer *a, AstNode *node) {
    const char *name = node->data.binding.name;

    /* Vars must be in component scope */
    Scope *comp_scope = scope_get_component(a->scope);
    if (!comp_scope) {
        analyzer_error(a, node->line, node->column,
                       "var '%s' must be inside a component", name);
    }

    /* Check for redefinition */
    if (scope_is_defined_local(a->scope, name)) {
        analyzer_error(a, node->line, node->column,
                       "Variable '%s' already defined", name);
    }

    /* Analyze value */
    Type *val_type = type_unknown(a->types);
    if (node->data.binding.value) {
        ExprResult val = analyze_expr(a, node->data.binding.value);
        val_type = val.type;
    }

    Symbol *sym = scope_define(a->scope, name, SYM_VAR, node, node->line, node->column);
    symbol_set_type(sym, val_type);
}

/* Analyze set statement */
static void analyze_set(Analyzer *a, AstNode *node) {
    /* Analyze target */
    ExprResult target = analyze_expr(a, node->data.set.target);

    /* Check if target is a var (mutable) */
    if (node->data.set.target->type == NODE_IDENT) {
        const char *name = node->data.set.target->data.ident.name;
        Symbol *sym = scope_lookup(a->scope, name);
        if (sym && sym->kind != SYM_VAR) {
            analyzer_error(a, node->line, node->column,
                           "Cannot assign to '%s' - only 'var' variables can be reassigned", name);
        }
    }

    /* Analyze value */
    ExprResult val = analyze_expr(a, node->data.set.value);

    /* Type check - value should be assignable to target */
    if (target.type->kind != TYPE_UNKNOWN && val.type->kind != TYPE_UNKNOWN) {
        if (!type_assignable(target.type, val.type)) {
            analyzer_error(a, node->line, node->column,
                           "Cannot assign %s to %s",
                           type_to_string(a->types, val.type),
                           type_to_string(a->types, target.type));
        }
    }
}

/* Analyze for loop */
static void analyze_for(Analyzer *a, AstNode *node) {
    /* Analyze iterable */
    ExprResult iter = analyze_expr(a, node->data.for_loop.iterable);

    /* Check it's iterable */
    if (!type_is_iterable(iter.type) &&
        iter.type->kind != TYPE_ANY &&
        iter.type->kind != TYPE_UNKNOWN) {
        analyzer_error(a, node->line, node->column,
                       "For loop requires iterable, got %s",
                       type_to_string(a->types, iter.type));
    }

    /* Create loop scope */
    analyzer_push_scope(a, SCOPE_FOR);

    /* Define loop variable */
    Type *item_type = type_element_of(iter.type);
    if (!item_type) item_type = type_any(a->types);

    Symbol *item = scope_define(a->scope, node->data.for_loop.item, SYM_PARAM,
                                 node, node->line, node->column);
    symbol_set_type(item, item_type);

    /* Define index variable if present */
    if (node->data.for_loop.index) {
        Symbol *idx = scope_define(a->scope, node->data.for_loop.index, SYM_PARAM,
                                   node, node->line, node->column);
        symbol_set_type(idx, type_int(a->types));
    }

    /* Analyze body */
    for (AstNode *child = node->data.for_loop.body; child; child = child->next) {
        analyze_node(a, child);
    }

    analyzer_pop_scope(a);
}

/* Analyze if statement */
static void analyze_if(Analyzer *a, AstNode *node) {
    /* Analyze condition */
    ExprResult cond = analyze_expr(a, node->data.if_stmt.condition);

    if (cond.type->kind != TYPE_BOOL &&
        cond.type->kind != TYPE_ANY &&
        cond.type->kind != TYPE_UNKNOWN) {
        analyzer_error(a, node->line, node->column,
                       "If condition must be boolean, got %s",
                       type_to_string(a->types, cond.type));
    }

    /* Analyze then branch */
    analyzer_push_scope(a, SCOPE_IF);
    for (AstNode *child = node->data.if_stmt.then_body; child; child = child->next) {
        analyze_node(a, child);
    }
    analyzer_pop_scope(a);

    /* Analyze elsif/else branches.
     * Parser stores elsif chains as nested if_stmt.else_branch links. */
    for (AstNode *branch = node->data.if_stmt.else_branch; branch;) {
        AstNode *next_branch = branch->next;

        if (branch->type == NODE_ELSIF || branch->type == NODE_IF) {
            ExprResult elsif_cond = analyze_expr(a, branch->data.if_stmt.condition);
            (void)elsif_cond;  /* Type check already done */

            analyzer_push_scope(a, SCOPE_IF);
            for (AstNode *child = branch->data.if_stmt.then_body; child; child = child->next) {
                analyze_node(a, child);
            }
            analyzer_pop_scope(a);
            next_branch = branch->data.if_stmt.else_branch ?
                          branch->data.if_stmt.else_branch : branch->next;
        } else if (branch->type == NODE_ELSE) {
            analyzer_push_scope(a, SCOPE_IF);
            for (AstNode *child = branch->data.else_stmt.body; child; child = child->next) {
                analyze_node(a, child);
            }
            analyzer_pop_scope(a);
            next_branch = NULL;  /* else is terminal */
        }

        branch = next_branch;
    }
}

/* Analyze match statement */
static void analyze_match(Analyzer *a, AstNode *node) {
    ExprResult value = analyze_expr(a, node->data.match.value);
    bool seen_default = false;

    for (AstNode *case_node = node->data.match.cases; case_node; case_node = case_node->next) {
        if (case_node->type != NODE_CASE) {
            analyzer_error(a, case_node->line, case_node->column,
                           "Only <case> and <default> nodes are allowed inside <match>");
            continue;
        }

        if (case_node->data.case_stmt.is_default) {
            if (seen_default) {
                analyzer_error(a, case_node->line, case_node->column,
                               "Only one default case is allowed");
            }
            seen_default = true;
        } else if (case_node->data.case_stmt.pattern) {
            ExprResult pattern = analyze_expr(a, case_node->data.case_stmt.pattern);
            if (value.type->kind != TYPE_UNKNOWN &&
                pattern.type->kind != TYPE_UNKNOWN &&
                value.type->kind != TYPE_ANY &&
                pattern.type->kind != TYPE_ANY &&
                !type_assignable(value.type, pattern.type) &&
                !type_assignable(pattern.type, value.type)) {
                analyzer_error(a, case_node->line, case_node->column,
                               "Case pattern type does not match match value type");
            }
        }

        analyzer_push_scope(a, SCOPE_IF);
        for (AstNode *child = case_node->data.case_stmt.body; child; child = child->next) {
            analyze_node(a, child);
        }
        analyzer_pop_scope(a);
    }
}

/* Analyze output */
static void analyze_output(Analyzer *a, AstNode *node) {
    if (node->data.output.expr) {
        ExprResult expr = analyze_expr(a, node->data.output.expr);
        (void)expr;  /* Output accepts any type */
    }
}

/* Analyze defcomp */
static void analyze_defcomp(Analyzer *a, AstNode *node) {
    const char *name = node->data.defcomp.name;

    /* Define component in current scope */
    if (scope_is_defined_local(a->scope, name)) {
        analyzer_error(a, node->line, node->column,
                       "Component '%s' already defined", name);
    }

    Symbol *comp = scope_define(a->scope, name, SYM_COMPONENT, node, node->line, node->column);

    /* Create component scope */
    analyzer_push_scope(a, SCOPE_COMPONENT);
    a->scope->component = comp;

    /* Define props */
    for (AstNode *prop = node->data.defcomp.props; prop; prop = prop->next) {
        if (prop->type == NODE_PROP_DEF) {
            const char *prop_name = prop->data.prop_def.name;
            Type *prop_type = analyzer_resolve_type_name(a, prop->data.prop_def.type_name);

            Symbol *prop_sym = scope_define(a->scope, prop_name, SYM_PARAM,
                                            prop, prop->line, prop->column);
            symbol_set_type(prop_sym, prop_type);

            /* Analyze default value if present */
            if (prop->data.prop_def.default_val) {
                ExprResult def = analyze_expr(a, prop->data.prop_def.default_val);
                if (prop_type->kind != TYPE_UNKNOWN && !type_assignable(prop_type, def.type)) {
                    analyzer_error(a, prop->line, prop->column,
                                   "Default value type mismatch");
                }
            }
        }
    }

    /* Define slots */
    for (AstNode *slot = node->data.defcomp.slots; slot; slot = slot->next) {
        if (slot->type == NODE_SLOT_DEF) {
            const char *slot_name = slot->data.slot_def.name;
            scope_define(a->scope, slot_name, SYM_SLOT, slot, slot->line, slot->column);
        }
    }

    /* Analyze body */
    for (AstNode *child = node->data.defcomp.body; child; child = child->next) {
        analyze_node(a, child);
    }

    analyzer_pop_scope(a);
}

/* Analyze import */
static void analyze_import(Analyzer *a, AstNode *node) {
    /* Define imported symbols */
    for (AstNode *name = node->data.import.names; name; name = name->next) {
        if (name->type == NODE_IDENT) {
            const char *sym_name = name->data.ident.name;

            if (scope_is_defined_local(a->scope, sym_name)) {
                analyzer_error(a, node->line, node->column,
                               "Import '%s' conflicts with existing definition", sym_name);
            }

            scope_define(a->scope, sym_name, SYM_IMPORT, node, node->line, node->column);
        }
    }
}

/* Analyze export */
static void analyze_export(Analyzer *a, AstNode *node) {
    if (!node->data.export.names) {
        analyzer_error(a, node->line, node->column,
                       "Export requires at least one symbol");
        return;
    }

    for (AstNode *name = node->data.export.names; name; name = name->next) {
        if (name->type != NODE_IDENT) continue;

        Symbol *sym = scope_lookup(a->scope, name->data.ident.name);
        if (!sym) {
            analyzer_error(a, name->line, name->column,
                           "Cannot export undefined symbol '%s'",
                           name->data.ident.name);
            continue;
        }
        symbol_set_flags(sym, SYM_FLAG_EXPORTED);
    }
}

/* Analyze interface declaration */
static void analyze_interface(Analyzer *a, AstNode *node) {
    /* For now interface is compile-time metadata only. */
    if (!node->data.interface.name || node->data.interface.name[0] == '\0') {
        analyzer_error(a, node->line, node->column, "Interface name cannot be empty");
    }

    for (AstNode *prop = node->data.interface.props; prop; prop = prop->next) {
        if (prop->type != NODE_PROP_DEF) continue;
        if (prop->data.prop_def.default_val) {
            (void)analyze_expr(a, prop->data.prop_def.default_val);
        }
    }
}

static AstNode *find_attr_by_name(AstNode *attrs, const char *name) {
    for (AstNode *attr = attrs; attr; attr = attr->next) {
        if (attr->type == NODE_ATTR && strcmp(attr->data.attr.name, name) == 0) {
            return attr;
        }
    }
    return NULL;
}

static void analyze_macro_invocation(Analyzer *a, AstNode *invocation, AstNode *macro_node) {
    analyzer_push_scope(a, SCOPE_MACRO);

    for (AstNode *param = macro_node->data.macro.params; param; param = param->next) {
        if (param->type != NODE_IDENT) continue;

        const char *param_name = param->data.ident.name;
        Type *param_type = type_any(a->types);
        AstNode *arg = find_attr_by_name(invocation->data.element.attrs, param_name);

        if (arg && arg->data.attr.value) {
            ExprResult arg_result = analyze_expr(a, arg->data.attr.value);
            param_type = arg_result.type;
        } else if (arg) {
            /* Shorthand argument: <macroName paramName> */
            Symbol *ref = scope_lookup(a->scope->parent, param_name);
            if (!ref) {
                analyzer_error(a, invocation->line, invocation->column,
                               "Undefined macro argument '%s'", param_name);
            } else {
                param_type = ref->type ? ref->type : type_any(a->types);
            }
        }

        Symbol *param_sym = scope_define(a->scope, param_name, SYM_PARAM, param,
                                         param->line, param->column);
        symbol_set_type(param_sym, param_type);
    }

    for (AstNode *child = macro_node->data.macro.body; child; child = child->next) {
        if (child->type == NODE_CHILDREN) {
            for (AstNode *passed = invocation->data.element.children; passed; passed = passed->next) {
                analyze_node(a, passed);
            }
        } else {
            analyze_node(a, child);
        }
    }

    analyzer_pop_scope(a);
}

/* Analyze macro declaration */
static void analyze_macro(Analyzer *a, AstNode *node) {
    const char *name = node->data.macro.name;
    if (scope_is_defined_local(a->scope, name)) {
        analyzer_error(a, node->line, node->column,
                       "Macro '%s' already defined", name);
        return;
    }
    scope_define(a->scope, name, SYM_MACRO, node, node->line, node->column);
}

/* Analyze mutation constructs */
static void analyze_insert(Analyzer *a, AstNode *node) {
    /* Verify target binding exists */
    Symbol *sym = scope_lookup(a->scope, node->data.insert.target);
    if (!sym) {
        analyzer_error(a, node->line, node->column,
                       "Unknown binding '%s' in insert", node->data.insert.target);
    }
    /* Analyze body children */
    for (AstNode *child = node->data.insert.body; child; child = child->next) {
        analyze_node(a, child);
    }
}

static void analyze_update(Analyzer *a, AstNode *node) {
    Symbol *sym = scope_lookup(a->scope, node->data.update.target);
    if (!sym) {
        analyzer_error(a, node->line, node->column,
                       "Unknown binding '%s' in update", node->data.update.target);
    }
    if (node->data.update.where) {
        ExprResult r = analyze_expr(a, node->data.update.where);
        (void)r;
    }
    for (AstNode *child = node->data.update.body; child; child = child->next) {
        analyze_node(a, child);
    }
}

static void analyze_delete(Analyzer *a, AstNode *node) {
    Symbol *sym = scope_lookup(a->scope, node->data.delete_stmt.target);
    if (!sym) {
        analyzer_error(a, node->line, node->column,
                       "Unknown binding '%s' in delete", node->data.delete_stmt.target);
    }
    if (node->data.delete_stmt.where) {
        ExprResult r = analyze_expr(a, node->data.delete_stmt.where);
        (void)r;
    }
    for (AstNode *child = node->data.delete_stmt.body; child; child = child->next) {
        analyze_node(a, child);
    }
}

static void analyze_bound_input(Analyzer *a, AstNode *node) {
    /* Check if object name resolves — it's optional for <insert> where
     * there's no existing record, only for <update> where we read values. */
    (void)scope_lookup(a->scope, node->data.bound_input.object_name);
    /* Analyze extra attrs */
    for (AstNode *attr = node->data.bound_input.attrs; attr; attr = attr->next) {
        if (attr->type == NODE_ATTR && attr->data.attr.value) {
            ExprResult val = analyze_expr(a, attr->data.attr.value);
            (void)val;
        }
    }
}

/* Analyze HTML element */
static void analyze_element(Analyzer *a, AstNode *node) {
    Symbol *macro_sym = scope_lookup(a->scope, node->data.element.tag);
    if (macro_sym && macro_sym->kind == SYM_MACRO && macro_sym->def_node) {
        analyze_macro_invocation(a, node, macro_sym->def_node);
        return;
    }

    /* Analyze attributes */
    for (AstNode *attr = node->data.element.attrs; attr; attr = attr->next) {
        if (attr->type == NODE_ATTR && attr->data.attr.value) {
            ExprResult val = analyze_expr(a, attr->data.attr.value);
            (void)val;
        }
    }

    /* Analyze children */
    for (AstNode *child = node->data.element.children; child; child = child->next) {
        analyze_node(a, child);
    }
}

/* Analyze node dispatch */
static void analyze_node(Analyzer *a, AstNode *node) {
    if (!node) return;

    switch (node->type) {
        case NODE_LET:
            analyze_let(a, node);
            break;
        case NODE_VAR:
            analyze_var(a, node);
            break;
        case NODE_SET:
            analyze_set(a, node);
            break;
        case NODE_FOR:
            analyze_for(a, node);
            break;
        case NODE_IF:
            analyze_if(a, node);
            break;
        case NODE_MATCH:
            analyze_match(a, node);
            break;
        case NODE_OUTPUT:
            analyze_output(a, node);
            break;
        case NODE_DEFCOMP:
            analyze_defcomp(a, node);
            break;
        case NODE_MACRO:
            analyze_macro(a, node);
            break;
        case NODE_IMPORT:
            analyze_import(a, node);
            break;
        case NODE_EXPORT:
            analyze_export(a, node);
            break;
        case NODE_INTERFACE:
            analyze_interface(a, node);
            break;
        case NODE_REQUIRE_AUTH:
            /* Directive - nothing to analyze, just a flag for bytecode */
            break;
        case NODE_INSERT:
            analyze_insert(a, node);
            break;
        case NODE_UPDATE:
            analyze_update(a, node);
            break;
        case NODE_DELETE:
            analyze_delete(a, node);
            break;
        case NODE_BOUND_INPUT:
            analyze_bound_input(a, node);
            break;
        case NODE_ELEMENT:
            analyze_element(a, node);
            break;
        case NODE_TEXT:
        case NODE_COMMENT:
            /* Nothing to analyze */
            break;
        default:
            /* Expression nodes handled elsewhere */
            break;
    }
}

/* Main entry point */
bool analyzer_analyze(Analyzer *a, AstNode *doc) {
    if (!doc || doc->type != NODE_DOCUMENT) {
        analyzer_error(a, 0, 0, "Expected document node");
        return false;
    }

    /* Create global scope */
    a->global_scope = scope_new(a->arena, SCOPE_GLOBAL, NULL);
    a->scope = a->global_scope;

    /* Register built-ins */
    analyzer_register_builtins(a);

    /* Analyze all children */
    for (AstNode *child = doc->data.document.children; child; child = child->next) {
        analyze_node(a, child);
    }

    return analyzer_ok(a);
}

ExprResult analyzer_analyze_expr(Analyzer *a, AstNode *expr) {
    return analyze_expr(a, expr);
}

void analyzer_analyze_node(Analyzer *a, AstNode *node) {
    analyze_node(a, node);
}
