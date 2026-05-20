/*
 * Compiler implementation
 */

#include "compiler.h"
#include "partial_eval.h"
#include <stdio.h>
#include <string.h>
#include <stdarg.h>
#include <stdlib.h>
#include <ctype.h>

/* Forward declarations */
static void compile_node(Compiler *c, AstNode *node);
static void compile_expr(Compiler *c, AstNode *expr);
static bool build_bindable_output_path(AstNode *expr, char *out, size_t cap, size_t *len);
static void record_set_target_marker(Compiler *c, AstNode *target);
static bool reactive_expr_is_supported(AstNode *expr);
static bool build_reactive_expr_program(Compiler *c, AstNode *expr, char **out_program);

/* Helper to emit a byte */
static void emit_byte(Compiler *c, uint8_t byte) {
    chunk_write(c->current_chunk, byte, c->current_line, c->arena);
}

/* Helper to emit two bytes */
static void emit_bytes(Compiler *c, uint8_t b1, uint8_t b2) {
    emit_byte(c, b1);
    emit_byte(c, b2);
}

/* Emit a u16 operand */
static void emit_u16(Compiler *c, uint16_t value) {
    chunk_write_u16(c->current_chunk, value, c->current_line, c->arena);
}

/* Emit opcode with u16 operand */
static void emit_op_u16(Compiler *c, OpCode op, uint16_t operand) {
    emit_byte(c, op);
    emit_u16(c, operand);
}

/* Emit a jump instruction, return offset for patching */
static int emit_jump(Compiler *c, OpCode op) {
    emit_byte(c, op);
    emit_bytes(c, 0xFF, 0xFF);  /* Placeholder */
    return chunk_current_offset(c->current_chunk) - 2;
}

/* Patch a jump instruction */
static void patch_jump(Compiler *c, int offset) {
    int jump = chunk_current_offset(c->current_chunk) - offset - 2;
    if (jump > 32767 || jump < -32768) {
        c->had_error = true;
        c->error_msg = "Jump too large";
        return;
    }
    chunk_patch_jump(c->current_chunk, offset, (int16_t)jump);
}

/* Emit a loop (backward jump) */
static void emit_loop(Compiler *c, int loop_start) {
    emit_byte(c, BC_JUMP);
    int offset = chunk_current_offset(c->current_chunk) - loop_start + 2;
    emit_u16(c, (uint16_t)(-offset));
}

/* Error reporting */
static void compile_error(Compiler *c, int line, int col, const char *fmt, ...) {
    if (c->had_error) return;  /* Only report first error */

    c->had_error = true;
    c->error_line = line;
    c->error_col = col;

    static char buf[256];
    va_list args;
    va_start(args, fmt);
    vsnprintf(buf, sizeof(buf), fmt, args);
    va_end(args);

    c->error_msg = arena_strdup(c->arena, buf);
}

/* Scope management */
static void begin_scope(Compiler *c) {
    c->scope_depth++;
    if (c->partial_eval && c->pe) {
        pe_push_scope(c->pe);
    }
}

static void end_scope(Compiler *c) {
    c->scope_depth--;

    /* Pop locals that went out of scope */
    while (c->scope->locals &&
           c->scope->locals->depth > c->scope_depth) {
        emit_byte(c, BC_POP);
        c->scope->locals = c->scope->locals->next;
        c->scope->local_count--;
    }

    if (c->partial_eval && c->pe) {
        pe_pop_scope(c->pe);
    }
}

/* Add a local variable */
static uint16_t add_local(Compiler *c, const char *name) {
    Local *local = arena_alloc(c->arena, sizeof(Local));
    local->name = name;
    local->depth = c->scope_depth;
    local->slot = (uint16_t)c->scope->local_count;
    local->is_captured = false;
    local->next = c->scope->locals;

    c->scope->locals = local;
    c->scope->local_count++;

    if (c->partial_eval && c->pe) {
        /* Locals default to dynamic for safe shadowing. */
        pe_define(c->pe, name, pe_unknown(), false);
    }

    if (local->slot >= 100) {
        fprintf(stderr, "[debug] add_local('%s') assigned slot %d (unusual!)\n", name, local->slot);
    }

    return local->slot;
}

/* Resolve a local variable */
static int resolve_local(Compiler *c, const char *name) {
    for (Local *local = c->scope->locals; local; local = local->next) {
        if (strcmp(local->name, name) == 0) {
            if (local->slot >= 100) {
                fprintf(stderr, "[debug] resolve_local('%s') = %d (unusual!)\n", name, local->slot);
            }
            return local->slot;
        }
    }
    return -1;
}

/* Add a string constant */
static uint16_t make_string_constant(Compiler *c, const char *str, size_t len) {
    uint16_t str_idx = bytecode_add_string(c->module, str, (uint32_t)len);

    Constant constant;
    constant.type = CONST_STRING;
    constant.v.string.data = c->module->strings[str_idx];
    constant.v.string.length = (uint32_t)len;

    return bytecode_add_constant(c->module, constant);
}

/* Compile a literal */
static void compile_literal(Compiler *c, AstNode *node) {
    c->current_line = node->line;

    switch (node->data.literal.lit_type) {
        case LIT_NULL:
            emit_byte(c, BC_NULL);
            break;

        case LIT_BOOL:
            emit_byte(c, node->data.literal.v.boolean ? BC_TRUE : BC_FALSE);
            break;

        case LIT_INT: {
            int64_t val = node->data.literal.v.integer;
            if (val >= -32768 && val <= 32767) {
                /* Use short form */
                emit_byte(c, BC_INT);
                emit_u16(c, (uint16_t)(int16_t)val);
            } else {
                /* Use constant */
                uint16_t idx = bytecode_add_int(c->module, val);
                emit_op_u16(c, BC_CONST, idx);
            }
            break;
        }

        case LIT_NUMBER: {
            uint16_t idx = bytecode_add_number(c->module, node->data.literal.v.number);
            emit_op_u16(c, BC_CONST, idx);
            break;
        }

        case LIT_STRING: {
            uint16_t idx = make_string_constant(c,
                node->data.literal.v.string.value,
                node->data.literal.v.string.length);
            emit_op_u16(c, BC_CONST, idx);
            break;
        }
    }
}

/* Compile an identifier reference */
static void compile_ident(Compiler *c, AstNode *node) {
    c->current_line = node->line;

    const char *name = node->data.ident.name;

    /* Check for local */
    int slot = resolve_local(c, name);
    if (slot >= 0) {
        emit_op_u16(c, BC_LOAD, (uint16_t)slot);
        return;
    }

    /* Check for builtin */
    int builtin = bytecode_find_builtin(c->module, name);
    if (builtin >= 0) {
        /* Builtins are only called, not loaded as values */
        compile_error(c, node->line, node->column,
                      "Cannot use builtin '%s' as value", name);
        return;
    }

    /* Must be a global or error */
    uint16_t name_idx = bytecode_add_string(c->module, name, (uint32_t)strlen(name));
    emit_op_u16(c, BC_LOAD_GLOBAL, name_idx);
}

/* Compile member access */
static void compile_member(Compiler *c, AstNode *node) {
    c->current_line = node->line;

    /* Compile object */
    compile_expr(c, node->data.member.object);

    /* Emit field access */
    const char *field = node->data.member.member;
    uint16_t field_idx = bytecode_add_string(c->module, field, (uint32_t)strlen(field));
    emit_op_u16(c, BC_LOAD_FIELD, field_idx);
}

/* Compile index access */
static void compile_index(Compiler *c, AstNode *node) {
    c->current_line = node->line;

    compile_expr(c, node->data.index.object);
    compile_expr(c, node->data.index.index);
    emit_byte(c, BC_LOAD_INDEX);
}

/* Compile binary expression */
static void compile_binary(Compiler *c, AstNode *node) {
    c->current_line = node->line;

    OpType op = node->data.binary.op;

    /* Short-circuit for AND/OR */
    if (op == OP_AND) {
        compile_expr(c, node->data.binary.left);
        int jump = emit_jump(c, BC_JUMP_IF_FALSE);
        emit_byte(c, BC_POP);
        compile_expr(c, node->data.binary.right);
        patch_jump(c, jump);
        return;
    }

    if (op == OP_OR) {
        compile_expr(c, node->data.binary.left);
        int jump = emit_jump(c, BC_JUMP_IF_TRUE);
        emit_byte(c, BC_POP);
        compile_expr(c, node->data.binary.right);
        patch_jump(c, jump);
        return;
    }

    /* Regular binary ops */
    compile_expr(c, node->data.binary.left);
    compile_expr(c, node->data.binary.right);

    switch (op) {
        case OP_ADD: emit_byte(c, BC_ADD); break;
        case OP_SUB: emit_byte(c, BC_SUB); break;
        case OP_MUL: emit_byte(c, BC_MUL); break;
        case OP_DIV: emit_byte(c, BC_DIV); break;
        case OP_MOD: emit_byte(c, BC_MOD); break;
        case OP_LT:  emit_byte(c, BC_LT); break;
        case OP_LTE: emit_byte(c, BC_LTE); break;
        case OP_GT:  emit_byte(c, BC_GT); break;
        case OP_GTE: emit_byte(c, BC_GTE); break;
        case OP_EQ:  emit_byte(c, BC_EQ); break;
        case OP_NEQ: emit_byte(c, BC_NEQ); break;
        default:
            compile_error(c, node->line, node->column, "Unknown binary operator");
    }
}

/* Compile unary expression */
static void compile_unary(Compiler *c, AstNode *node) {
    c->current_line = node->line;

    compile_expr(c, node->data.unary.operand);

    switch (node->data.unary.op) {
        case OP_NEG: emit_byte(c, BC_NEG); break;
        case OP_NOT: emit_byte(c, BC_NOT); break;
        default:
            compile_error(c, node->line, node->column, "Unknown unary operator");
    }
}

/* Compile ternary expression */
static void compile_ternary(Compiler *c, AstNode *node) {
    c->current_line = node->line;

    compile_expr(c, node->data.ternary.condition);
    int else_jump = emit_jump(c, BC_JUMP_IF_FALSE);
    emit_byte(c, BC_POP);

    compile_expr(c, node->data.ternary.then_expr);
    int end_jump = emit_jump(c, BC_JUMP);

    patch_jump(c, else_jump);
    emit_byte(c, BC_POP);
    compile_expr(c, node->data.ternary.else_expr);
    patch_jump(c, end_jump);
}

/* Compile function call */
static void compile_call(Compiler *c, AstNode *node) {
    c->current_line = node->line;

    const char *name = node->data.call.name;

    /* Count arguments */
    int argc = 0;
    for (AstNode *arg = node->data.call.args; arg; arg = arg->next) {
        compile_expr(c, arg);
        argc++;
    }

    /* Check for builtin */
    int builtin = bytecode_find_builtin(c->module, name);
    if (builtin >= 0) {
        emit_byte(c, BC_CALL_BUILTIN);
        emit_u16(c, (uint16_t)builtin);
        emit_byte(c, (uint8_t)argc);
        return;
    }

    /* User function */
    uint16_t name_idx = bytecode_add_string(c->module, name, (uint32_t)strlen(name));
    emit_byte(c, BC_CALL);
    emit_u16(c, name_idx);
    emit_byte(c, (uint8_t)argc);
}

/* Compile pipe expression */
static void compile_pipe(Compiler *c, AstNode *node) {
    c->current_line = node->line;

    /* Compile input expression */
    compile_expr(c, node->data.pipe.input);

    /* Apply each pipe stage */
    for (AstNode *stage = node->data.pipe.stages; stage; stage = stage->next) {
        if (stage->type == NODE_CALL) {
            const char *fn_name = stage->data.call.name;

            /* Compile additional arguments */
            int extra_argc = 0;
            for (AstNode *arg = stage->data.call.args; arg; arg = arg->next) {
                compile_expr(c, arg);
                extra_argc++;
            }

            /* Check for builtin */
            int builtin = bytecode_find_builtin(c->module, fn_name);
            if (builtin >= 0) {
                emit_byte(c, BC_CALL_BUILTIN);
                emit_u16(c, (uint16_t)builtin);
                emit_byte(c, (uint8_t)(1 + extra_argc));  /* piped value + extra args */
            } else {
                uint16_t name_idx = bytecode_add_string(c->module, fn_name,
                                                        (uint32_t)strlen(fn_name));
                emit_byte(c, BC_CALL);
                emit_u16(c, name_idx);
                emit_byte(c, (uint8_t)(1 + extra_argc));
            }
        }
    }
}

/* Compile array literal */
static void compile_array(Compiler *c, AstNode *node) {
    c->current_line = node->line;

    int count = 0;
    for (AstNode *elem = node->data.array.elements; elem; elem = elem->next) {
        compile_expr(c, elem);
        count++;
    }

    emit_op_u16(c, BC_ARRAY_NEW, (uint16_t)count);
}

/* Compile object literal */
static void compile_object(Compiler *c, AstNode *node) {
    c->current_line = node->line;

    int count = 0;
    for (AstNode *pair = node->data.object.pairs; pair; pair = pair->next) {
        /* Pairs should be key-value nodes */
        /* For now, assume pairs are structured appropriately */
        count++;
    }

    emit_op_u16(c, BC_OBJECT_NEW, (uint16_t)count);

    /* Set fields */
    for (AstNode *pair = node->data.object.pairs; pair; pair = pair->next) {
        if (pair->type == NODE_ATTR) {
            /* key: value */
            const char *key = pair->data.attr.name;
            uint16_t key_idx = bytecode_add_string(c->module, key, (uint32_t)strlen(key));
            compile_expr(c, pair->data.attr.value);
            emit_op_u16(c, BC_OBJECT_SET, key_idx);
        }
    }
}

static bool pe_value_can_emit(const PEValue *value) {
    if (!value) return false;

    switch (value->kind) {
        case PE_NULL:
        case PE_BOOL:
        case PE_INT:
        case PE_NUMBER:
        case PE_STRING:
            return true;
        case PE_ARRAY:
            for (uint32_t i = 0; i < value->v.array.count; i++) {
                if (!pe_value_can_emit(&value->v.array.elements[i])) {
                    return false;
                }
            }
            return true;
        default:
            return false;
    }
}

static void emit_pe_value(Compiler *c, const PEValue *value) {
    switch (value->kind) {
        case PE_NULL:
            emit_byte(c, BC_NULL);
            break;
        case PE_BOOL:
            emit_byte(c, value->v.boolean ? BC_TRUE : BC_FALSE);
            break;
        case PE_INT:
            if (value->v.integer >= -32768 && value->v.integer <= 32767) {
                emit_byte(c, BC_INT);
                emit_u16(c, (uint16_t)(int16_t)value->v.integer);
            } else {
                uint16_t idx = bytecode_add_int(c->module, value->v.integer);
                emit_op_u16(c, BC_CONST, idx);
            }
            break;
        case PE_NUMBER: {
            uint16_t idx = bytecode_add_number(c->module, value->v.number);
            emit_op_u16(c, BC_CONST, idx);
            break;
        }
        case PE_STRING: {
            uint16_t idx = make_string_constant(c, value->v.string.data, value->v.string.length);
            emit_op_u16(c, BC_CONST, idx);
            break;
        }
        case PE_ARRAY:
            for (uint32_t i = 0; i < value->v.array.count; i++) {
                emit_pe_value(c, &value->v.array.elements[i]);
            }
            emit_op_u16(c, BC_ARRAY_NEW, (uint16_t)value->v.array.count);
            break;
        default:
            /* Caller must guard with pe_value_can_emit. */
            emit_byte(c, BC_NULL);
            break;
    }
}

/* Mark an existing binding as dynamic after a mutation. */
static void pe_invalidate_binding(Compiler *c, const char *name) {
    if (!c->partial_eval || !c->pe || !name) return;

    for (PEScope *scope = c->pe->scope; scope; scope = scope->parent) {
        for (PEBinding *binding = scope->bindings; binding; binding = binding->next) {
            if (strcmp(binding->name, name) == 0) {
                binding->is_static = false;
                binding->value = pe_unknown();
                return;
            }
        }
    }

    pe_define(c->pe, name, pe_unknown(), false);
}

/* Compile expression */
static void compile_expr(Compiler *c, AstNode *expr) {
    if (!expr) {
        emit_byte(c, BC_NULL);
        return;
    }

    if (c->partial_eval && c->pe) {
        PEValue value = pe_eval(c->pe, expr);
        if (pe_is_static(&value) && pe_value_can_emit(&value)) {
            c->current_line = expr->line;
            emit_pe_value(c, &value);
            return;
        }
    }

    switch (expr->type) {
        case NODE_LITERAL:
            compile_literal(c, expr);
            break;
        case NODE_IDENT:
            compile_ident(c, expr);
            break;
        case NODE_MEMBER:
            compile_member(c, expr);
            break;
        case NODE_INDEX:
            compile_index(c, expr);
            break;
        case NODE_BINARY:
            compile_binary(c, expr);
            break;
        case NODE_UNARY:
            compile_unary(c, expr);
            break;
        case NODE_TERNARY:
            compile_ternary(c, expr);
            break;
        case NODE_CALL:
            compile_call(c, expr);
            break;
        case NODE_PIPE:
            compile_pipe(c, expr);
            break;
        case NODE_ARRAY:
            compile_array(c, expr);
            break;
        case NODE_OBJECT:
            compile_object(c, expr);
            break;
        default:
            compile_error(c, expr->line, expr->column,
                          "Cannot compile expression type %d", expr->type);
    }
}

static void sql_append(Arena *arena, const char *str, char **out, size_t *len, size_t *cap) {
    size_t slen = strlen(str);
    while (*len + slen + 1 > *cap) {
        size_t new_cap = *cap * 2;
        char *new_out = arena_alloc(arena, new_cap);
        memcpy(new_out, *out, *len);
        *out = new_out;
        *cap = new_cap;
    }
    memcpy(*out + *len, str, slen);
    *len += slen;
    (*out)[*len] = '\0';
}

static void reactive_append_encoded(Arena *arena, const char *str, char **out, size_t *len, size_t *cap) {
    if (!str) return;
    for (const unsigned char *p = (const unsigned char *)str; *p; p++) {
        unsigned char ch = *p;
        if (isalnum(ch) || ch == '_' || ch == '-' || ch == '.') {
            char tmp[2] = {(char)ch, '\0'};
            sql_append(arena, tmp, out, len, cap);
        } else {
            char tmp[4];
            snprintf(tmp, sizeof(tmp), "%%%02X", ch);
            sql_append(arena, tmp, out, len, cap);
        }
    }
}

static void reactive_append_token(Arena *arena, const char *token, char **out, size_t *len, size_t *cap) {
    sql_append(arena, token, out, len, cap);
    sql_append(arena, ";", out, len, cap);
}

static void reactive_append_token_value(Arena *arena, const char *prefix, const char *value,
                                        char **out, size_t *len, size_t *cap) {
    sql_append(arena, prefix, out, len, cap);
    reactive_append_encoded(arena, value ? value : "", out, len, cap);
    sql_append(arena, ";", out, len, cap);
}

static const char *reactive_binary_op_name(OpType op) {
    switch (op) {
        case OP_ADD: return "add";
        case OP_SUB: return "sub";
        case OP_MUL: return "mul";
        case OP_DIV: return "div";
        case OP_MOD: return "mod";
        case OP_LT: return "lt";
        case OP_LTE: return "lte";
        case OP_GT: return "gt";
        case OP_GTE: return "gte";
        case OP_EQ: return "eq";
        case OP_NEQ: return "neq";
        case OP_AND: return "and";
        case OP_OR: return "or";
        default: return NULL;
    }
}

static const char *reactive_unary_op_name(OpType op) {
    switch (op) {
        case OP_NEG: return "neg";
        case OP_NOT: return "not";
        default: return NULL;
    }
}

static bool reactive_expr_is_supported(AstNode *expr) {
    if (!expr) return false;

    switch (expr->type) {
        case NODE_LITERAL:
        case NODE_IDENT:
            return true;
        case NODE_MEMBER:
            return reactive_expr_is_supported(expr->data.member.object);
        case NODE_INDEX:
            return reactive_expr_is_supported(expr->data.index.object) &&
                   reactive_expr_is_supported(expr->data.index.index);
        case NODE_BINARY:
            return reactive_binary_op_name(expr->data.binary.op) != NULL &&
                   reactive_expr_is_supported(expr->data.binary.left) &&
                   reactive_expr_is_supported(expr->data.binary.right);
        case NODE_UNARY:
            return reactive_unary_op_name(expr->data.unary.op) != NULL &&
                   reactive_expr_is_supported(expr->data.unary.operand);
        case NODE_TERNARY:
            return reactive_expr_is_supported(expr->data.ternary.condition) &&
                   reactive_expr_is_supported(expr->data.ternary.then_expr) &&
                   reactive_expr_is_supported(expr->data.ternary.else_expr);
        default:
            return false;
    }
}

static bool reactive_serialize_expr(Compiler *c, AstNode *expr, char **out, size_t *len, size_t *cap) {
    if (!expr) return false;

    switch (expr->type) {
        case NODE_LITERAL: {
            switch (expr->data.literal.lit_type) {
                case LIT_NULL:
                    reactive_append_token(c->arena, "Z", out, len, cap);
                    return true;
                case LIT_BOOL:
                    reactive_append_token(c->arena,
                        expr->data.literal.v.boolean ? "B:1" : "B:0", out, len, cap);
                    return true;
                case LIT_INT: {
                    char buf[64];
                    snprintf(buf, sizeof(buf), "%lld", (long long)expr->data.literal.v.integer);
                    reactive_append_token_value(c->arena, "I:", buf, out, len, cap);
                    return true;
                }
                case LIT_NUMBER: {
                    char buf[64];
                    snprintf(buf, sizeof(buf), "%.17g", expr->data.literal.v.number);
                    reactive_append_token_value(c->arena, "N:", buf, out, len, cap);
                    return true;
                }
                case LIT_STRING:
                    reactive_append_token_value(c->arena, "S:",
                                                expr->data.literal.v.string.value, out, len, cap);
                    return true;
            }
            return false;
        }
        case NODE_IDENT:
            reactive_append_token_value(c->arena, "P:", expr->data.ident.name, out, len, cap);
            return true;
        case NODE_MEMBER: {
            char path[192] = {0};
            size_t path_len = 0;
            if (build_bindable_output_path(expr, path, sizeof(path), &path_len)) {
                reactive_append_token_value(c->arena, "P:", path, out, len, cap);
                return true;
            }
            if (!reactive_serialize_expr(c, expr->data.member.object, out, len, cap)) return false;
            reactive_append_token_value(c->arena, "M:", expr->data.member.member, out, len, cap);
            return true;
        }
        case NODE_INDEX:
            if (!reactive_serialize_expr(c, expr->data.index.object, out, len, cap)) return false;
            if (!reactive_serialize_expr(c, expr->data.index.index, out, len, cap)) return false;
            reactive_append_token(c->arena, "X", out, len, cap);
            return true;
        case NODE_BINARY: {
            const char *op_name = reactive_binary_op_name(expr->data.binary.op);
            if (!op_name) return false;
            if (!reactive_serialize_expr(c, expr->data.binary.left, out, len, cap)) return false;
            if (!reactive_serialize_expr(c, expr->data.binary.right, out, len, cap)) return false;
            reactive_append_token_value(c->arena, "O:", op_name, out, len, cap);
            return true;
        }
        case NODE_UNARY: {
            const char *op_name = reactive_unary_op_name(expr->data.unary.op);
            if (!op_name) return false;
            if (!reactive_serialize_expr(c, expr->data.unary.operand, out, len, cap)) return false;
            reactive_append_token_value(c->arena, "U:", op_name, out, len, cap);
            return true;
        }
        case NODE_TERNARY:
            if (!reactive_serialize_expr(c, expr->data.ternary.condition, out, len, cap)) return false;
            if (!reactive_serialize_expr(c, expr->data.ternary.then_expr, out, len, cap)) return false;
            if (!reactive_serialize_expr(c, expr->data.ternary.else_expr, out, len, cap)) return false;
            reactive_append_token(c->arena, "T", out, len, cap);
            return true;
        default:
            return false;
    }
}

static bool build_reactive_expr_program(Compiler *c, AstNode *expr, char **out_program) {
    if (!c || !expr || !out_program) return false;
    if (!reactive_expr_is_supported(expr)) return false;

    size_t cap = 256;
    size_t len = 0;
    char *out = arena_alloc(c->arena, cap);
    out[0] = '\0';

    if (!reactive_serialize_expr(c, expr, &out, &len, &cap) || len == 0) {
        return false;
    }
    *out_program = out;
    return true;
}

/* Build a serialized action program from <on> handler body.
 * Format: "SET:<path>\nPROG:<expr-tokens>\n" per action.
 * Multiple actions separated by blank line. */
static bool build_on_action_program(Compiler *c, AstNode *body, char **out_result) {
    if (!c || !body || !out_result) return false;

    size_t cap = 512;
    size_t len = 0;
    char *out = arena_alloc(c->arena, cap);
    out[0] = '\0';

    bool has_action = false;

    for (AstNode *child = body; child; child = child->next) {
        if (child->type != NODE_SET) continue;

        /* Build target path */
        char target_path[192] = {0};
        size_t target_len = 0;
        if (!build_bindable_output_path(child->data.set.target,
                target_path, sizeof(target_path), &target_len)) {
            continue;
        }

        /* Build expression program */
        char *expr_prog = NULL;
        if (!build_reactive_expr_program(c, child->data.set.value, &expr_prog) || !expr_prog) {
            continue;
        }

        /* Separator between actions */
        if (has_action) {
            sql_append(c->arena, "\n", &out, &len, &cap);
        }

        /* Emit SET:<path>\nPROG:<program> */
        sql_append(c->arena, "SET:", &out, &len, &cap);
        sql_append(c->arena, target_path, &out, &len, &cap);
        sql_append(c->arena, "\nPROG:", &out, &len, &cap);
        sql_append(c->arena, expr_prog, &out, &len, &cap);

        has_action = true;
    }

    if (!has_action) return false;
    *out_result = out;
    return true;
}

/* Serialize SQL AST to query string */
static void sql_serialize_expr(Arena *arena, AstNode *expr, char **out, size_t *len, size_t *cap);

static void sql_serialize_expr(Arena *arena, AstNode *expr, char **out, size_t *len, size_t *cap) {
    if (!expr) return;

    switch (expr->type) {
        case NODE_LITERAL:
            if (expr->data.literal.lit_type == LIT_STRING) {
                sql_append(arena, "'", out, len, cap);
                sql_append(arena, expr->data.literal.v.string.value, out, len, cap);
                sql_append(arena, "'", out, len, cap);
            } else if (expr->data.literal.lit_type == LIT_INT) {
                char buf[32];
                snprintf(buf, sizeof(buf), "%lld", (long long)expr->data.literal.v.integer);
                sql_append(arena, buf, out, len, cap);
            } else if (expr->data.literal.lit_type == LIT_NUMBER) {
                char buf[32];
                snprintf(buf, sizeof(buf), "%g", expr->data.literal.v.number);
                sql_append(arena, buf, out, len, cap);
            } else if (expr->data.literal.lit_type == LIT_BOOL) {
                sql_append(arena, expr->data.literal.v.boolean ? "true" : "false", out, len, cap);
            } else {
                sql_append(arena, "NULL", out, len, cap);
            }
            break;
        case NODE_IDENT:
            sql_append(arena, expr->data.ident.name, out, len, cap);
            break;
        case NODE_BINARY: {
            sql_append(arena, "(", out, len, cap);
            sql_serialize_expr(arena, expr->data.binary.left, out, len, cap);
            switch (expr->data.binary.op) {
                case OP_ADD: sql_append(arena, " + ", out, len, cap); break;
                case OP_SUB: sql_append(arena, " - ", out, len, cap); break;
                case OP_MUL: sql_append(arena, " * ", out, len, cap); break;
                case OP_DIV: sql_append(arena, " / ", out, len, cap); break;
                case OP_EQ:  sql_append(arena, " = ", out, len, cap); break;
                case OP_NEQ: sql_append(arena, " <> ", out, len, cap); break;
                case OP_LT:  sql_append(arena, " < ", out, len, cap); break;
                case OP_LTE: sql_append(arena, " <= ", out, len, cap); break;
                case OP_GT:  sql_append(arena, " > ", out, len, cap); break;
                case OP_GTE: sql_append(arena, " >= ", out, len, cap); break;
                case OP_AND: sql_append(arena, " AND ", out, len, cap); break;
                case OP_OR:  sql_append(arena, " OR ", out, len, cap); break;
                default: sql_append(arena, " ? ", out, len, cap); break;
            }
            sql_serialize_expr(arena, expr->data.binary.right, out, len, cap);
            sql_append(arena, ")", out, len, cap);
            break;
        }
        case NODE_MEMBER:
            sql_serialize_expr(arena, expr->data.member.object, out, len, cap);
            sql_append(arena, ".", out, len, cap);
            sql_append(arena, expr->data.member.member, out, len, cap);
            break;
        case NODE_SQL_PARAM:
            sql_append(arena, ":", out, len, cap);
            sql_append(arena, expr->data.sql_param.name, out, len, cap);
            break;
        default:
            sql_append(arena, "?", out, len, cap);
            break;
    }
}

static char *sql_serialize(Compiler *c, AstNode *sql, size_t *out_len) {
    size_t cap = 256;
    size_t len = 0;
    char *out = arena_alloc(c->arena, cap);
    out[0] = '\0';

    sql_append(c->arena, "SELECT ", &out, &len, &cap);

    /* Columns */
    bool first = true;
    for (AstNode *col = sql->data.sql.columns; col; col = col->next) {
        if (!first) sql_append(c->arena, ", ", &out, &len, &cap);
        first = false;
        if (col->data.sql_column.expr->type == NODE_IDENT &&
            strcmp(col->data.sql_column.expr->data.ident.name, "*") == 0) {
            sql_append(c->arena, "*", &out, &len, &cap);
        } else {
            sql_serialize_expr(c->arena, col->data.sql_column.expr, &out, &len, &cap);
        }
        if (col->data.sql_column.alias) {
            sql_append(c->arena, " AS ", &out, &len, &cap);
            sql_append(c->arena, col->data.sql_column.alias, &out, &len, &cap);
        }
    }

    /* FROM */
    if (sql->data.sql.from) {
        sql_append(c->arena, " FROM ", &out, &len, &cap);
        sql_append(c->arena, sql->data.sql.from->data.sql_table.table, &out, &len, &cap);
        if (sql->data.sql.from->data.sql_table.alias) {
            sql_append(c->arena, " ", &out, &len, &cap);
            sql_append(c->arena, sql->data.sql.from->data.sql_table.alias, &out, &len, &cap);
        }
    }

    /* JOINs */
    for (AstNode *join = sql->data.sql.joins; join; join = join->next) {
        sql_append(c->arena, " JOIN ", &out, &len, &cap);
        sql_append(c->arena, join->data.sql_table.table, &out, &len, &cap);
        if (join->data.sql_table.alias) {
            sql_append(c->arena, " ", &out, &len, &cap);
            sql_append(c->arena, join->data.sql_table.alias, &out, &len, &cap);
        }
        if (join->data.sql_table.on_condition) {
            sql_append(c->arena, " ON ", &out, &len, &cap);
            sql_serialize_expr(c->arena, join->data.sql_table.on_condition, &out, &len, &cap);
        }
    }

    /* WHERE */
    if (sql->data.sql.where) {
        sql_append(c->arena, " WHERE ", &out, &len, &cap);
        sql_serialize_expr(c->arena, sql->data.sql.where, &out, &len, &cap);
    }

    /* ORDER BY */
    if (sql->data.sql.order) {
        sql_append(c->arena, " ORDER BY ", &out, &len, &cap);
        first = true;
        for (AstNode *ord = sql->data.sql.order; ord; ord = ord->next) {
            if (!first) sql_append(c->arena, ", ", &out, &len, &cap);
            first = false;
            sql_serialize_expr(c->arena, ord->data.sql_order.expr, &out, &len, &cap);
            if (ord->data.sql_order.descending) {
                sql_append(c->arena, " DESC", &out, &len, &cap);
            }
        }
    }

    /* LIMIT */
    if (sql->data.sql.limit) {
        sql_append(c->arena, " LIMIT ", &out, &len, &cap);
        sql_serialize_expr(c->arena, sql->data.sql.limit, &out, &len, &cap);
    }

    /* OFFSET */
    if (sql->data.sql.offset) {
        sql_append(c->arena, " OFFSET ", &out, &len, &cap);
        sql_serialize_expr(c->arena, sql->data.sql.offset, &out, &len, &cap);
    }

    *out_len = len;
    return out;
}

#define MAX_SQL_PARAMS 64

typedef struct {
    const char *names[MAX_SQL_PARAMS];
    uint16_t count;
} SqlParamList;

static void sql_collect_params_expr(AstNode *expr, SqlParamList *params) {
    if (!expr || !params) return;

    switch (expr->type) {
        case NODE_SQL_PARAM: {
            const char *name = expr->data.sql_param.name;
            for (uint16_t i = 0; i < params->count; i++) {
                if (strcmp(params->names[i], name) == 0) return;
            }
            if (params->count < MAX_SQL_PARAMS) {
                params->names[params->count++] = name;
            }
            return;
        }
        case NODE_BINARY:
            sql_collect_params_expr(expr->data.binary.left, params);
            sql_collect_params_expr(expr->data.binary.right, params);
            return;
        case NODE_UNARY:
            sql_collect_params_expr(expr->data.unary.operand, params);
            return;
        case NODE_TERNARY:
            sql_collect_params_expr(expr->data.ternary.condition, params);
            sql_collect_params_expr(expr->data.ternary.then_expr, params);
            sql_collect_params_expr(expr->data.ternary.else_expr, params);
            return;
        case NODE_MEMBER:
            sql_collect_params_expr(expr->data.member.object, params);
            return;
        case NODE_INDEX:
            sql_collect_params_expr(expr->data.index.object, params);
            sql_collect_params_expr(expr->data.index.index, params);
            return;
        case NODE_CALL:
            for (AstNode *arg = expr->data.call.args; arg; arg = arg->next) {
                sql_collect_params_expr(arg, params);
            }
            return;
        case NODE_PIPE:
            sql_collect_params_expr(expr->data.pipe.input, params);
            for (AstNode *stage = expr->data.pipe.stages; stage; stage = stage->next) {
                sql_collect_params_expr(stage, params);
            }
            return;
        case NODE_ARRAY:
            for (AstNode *it = expr->data.array.elements; it; it = it->next) {
                sql_collect_params_expr(it, params);
            }
            return;
        case NODE_OBJECT:
            for (AstNode *pair = expr->data.object.pairs; pair; pair = pair->next) {
                sql_collect_params_expr(pair->data.attr.value, params);
            }
            return;
        default:
            return;
    }
}

static void sql_collect_params(AstNode *sql, SqlParamList *params) {
    if (!sql || sql->type != NODE_SQL || !params) return;

    for (AstNode *col = sql->data.sql.columns; col; col = col->next) {
        sql_collect_params_expr(col->data.sql_column.expr, params);
    }
    for (AstNode *join = sql->data.sql.joins; join; join = join->next) {
        sql_collect_params_expr(join->data.sql_table.on_condition, params);
    }
    sql_collect_params_expr(sql->data.sql.where, params);
    for (AstNode *ord = sql->data.sql.order; ord; ord = ord->next) {
        sql_collect_params_expr(ord->data.sql_order.expr, params);
    }
    sql_collect_params_expr(sql->data.sql.limit, params);
    sql_collect_params_expr(sql->data.sql.offset, params);
}

/* Compile let binding */
static void compile_let(Compiler *c, AstNode *node) {
    c->current_line = node->line;

    const char *name = node->data.binding.name;
    bool has_static_binding = false;
    PEValue static_binding = pe_unknown();

    /* Compile value */
    if (node->data.binding.value) {
        if (c->partial_eval && c->pe) {
            static_binding = pe_eval(c->pe, node->data.binding.value);
            has_static_binding = pe_is_static(&static_binding);
        }
        compile_expr(c, node->data.binding.value);
    } else if (node->data.binding.sql) {
        /* SQL query - serialize and emit fetch */
        size_t query_len;
        char *query = sql_serialize(c, node->data.binding.sql, &query_len);
        SqlParamList params = { .count = 0 };
        uint16_t param_slots[MAX_SQL_PARAMS] = {0};
        sql_collect_params(node->data.binding.sql, &params);

        for (uint16_t i = 0; i < params.count; i++) {
            int slot = resolve_local(c, params.names[i]);
            if (slot < 0) {
                compile_error(c, node->line, node->column,
                              "Undefined SQL parameter '%s'", params.names[i]);
                param_slots[i] = 0;
            } else {
                param_slots[i] = (uint16_t)slot;
            }
        }

        uint16_t req_idx = bytecode_add_data_req(c->module, name, query, (uint32_t)query_len,
                                                  node->data.binding.single,
                                                  node->data.binding.dynamic,
                                                  (uint32_t)node->line,
                                                  (uint32_t)node->column,
                                                  params.count,
                                                  params.names,
                                                  param_slots);
        emit_op_u16(c, BC_FETCH_DATA, req_idx);
    } else {
        emit_byte(c, BC_NULL);
    }

    /* Define local */
    begin_scope(c);
    add_local(c, name);

    if (c->partial_eval && c->pe && has_static_binding) {
        pe_define(c->pe, name, static_binding, true);
    }

    /* Compile body */
    for (AstNode *child = node->data.binding.body; child; child = child->next) {
        compile_node(c, child);
    }

    end_scope(c);
}

/* Compile var binding */
static void compile_var(Compiler *c, AstNode *node) {
    /* var is different from let - it creates component-scoped state, not a new scope */
    c->current_line = node->line;

    const char *name = node->data.binding.name;

    /* Compile initial value */
    if (node->data.binding.value) {
        compile_expr(c, node->data.binding.value);
    } else {
        emit_byte(c, BC_NULL);
    }

    /* Add as local in current scope (NOT a new scope) */
    add_local(c, name);

    if (c->partial_eval && c->pe) {
        /* var is mutable at runtime; keep it dynamic for soundness. */
        pe_invalidate_binding(c, name);
    }
}

/* Compile set statement */
static void compile_set(Compiler *c, AstNode *node) {
    c->current_line = node->line;
    record_set_target_marker(c, node->data.set.target);

    /* Compile the new value first */
    compile_expr(c, node->data.set.value);

    /* Now handle the target */
    AstNode *target = node->data.set.target;

    if (target->type == NODE_IDENT) {
        /* Simple variable assignment */
        const char *name = target->data.ident.name;
        int slot = resolve_local(c, name);
        if (slot >= 0) {
            emit_op_u16(c, BC_STORE, (uint16_t)slot);
            pe_invalidate_binding(c, name);
        } else {
            /* Global or error */
            compile_error(c, node->line, node->column,
                          "Cannot set undefined variable '%s'", name);
        }
    } else if (target->type == NODE_MEMBER) {
        /* Member assignment: obj.field = value */
        /* Stack: value
         * Need to: push obj, then store field
         */
        compile_expr(c, target->data.member.object);

        const char *field = target->data.member.member;
        uint16_t field_idx = bytecode_add_string(c->module, field, (uint32_t)strlen(field));

        /* Emit STORE_FIELD: pops value and obj, stores value in obj.field */
        emit_op_u16(c, BC_STORE_FIELD, field_idx);

        if (target->data.member.object &&
            target->data.member.object->type == NODE_IDENT) {
            pe_invalidate_binding(c, target->data.member.object->data.ident.name);
        }
    } else if (target->type == NODE_INDEX) {
        /* Index assignment: arr[i] = value */
        /* Stack: value
         * Need to: push arr, push index, then store
         */
        compile_expr(c, target->data.index.object);
        compile_expr(c, target->data.index.index);

        emit_byte(c, BC_STORE_INDEX);

        if (target->data.index.object &&
            target->data.index.object->type == NODE_IDENT) {
            pe_invalidate_binding(c, target->data.index.object->data.ident.name);
        }
    } else {
        compile_error(c, node->line, node->column,
                      "Invalid assignment target");
    }
}

/* Helper to emit dependencies for a node */
static void emit_deps_start(Compiler *c, AstNode *expr) {
    if (!expr->deps || expr->deps->count == 0) return;

    /* Add each dependency path to the module and emit markers */
    for (DepPath *p = expr->deps->paths; p; p = p->next) {
        const char *path_str = dep_path_to_string(p);
        uint16_t dep_idx = bytecode_add_dependency(c->module, path_str);
        emit_op_u16(c, BC_DEP_START, dep_idx);
    }
}

static void emit_deps_end(Compiler *c, AstNode *expr) {
    if (!expr->deps || expr->deps->count == 0) return;

    /* Emit end marker for each dependency */
    for (int i = 0; i < expr->deps->count; i++) {
        emit_byte(c, BC_DEP_END);
    }
}

/* Build a dot-path from expressions that can be patched directly in-browser:
 *   ident        -> "counter"
 *   member chain -> "user.name"
 * Returns false for non-bindable expressions (binary ops, calls, indexes, etc). */
static bool build_bindable_output_path(AstNode *expr, char *out, size_t cap, size_t *len) {
    if (!expr || !out || !len || cap == 0) return false;

    if (expr->type == NODE_IDENT) {
        const char *name = expr->data.ident.name;
        if (!name || !name[0]) return false;
        size_t n = strlen(name);
        if (*len + n >= cap) return false;
        memcpy(out + *len, name, n);
        *len += n;
        out[*len] = '\0';
        return true;
    }

    if (expr->type == NODE_MEMBER) {
        if (!build_bindable_output_path(expr->data.member.object, out, cap, len)) {
            return false;
        }
        const char *member = expr->data.member.member;
        if (!member || !member[0]) return false;
        size_t m = strlen(member);
        if (*len + 1 + m >= cap) return false;
        out[(*len)++] = '.';
        memcpy(out + *len, member, m);
        *len += m;
        out[*len] = '\0';
        return true;
    }

    return false;
}

/* Emit synthetic dependency marker for direct output bindings.
 * This allows browser-side runtimes to patch only affected text nodes. */
static void emit_direct_var_bind_start(Compiler *c, AstNode *expr) {
    char path[192] = {0};
    size_t path_len = 0;
    if (!build_bindable_output_path(expr, path, sizeof(path), &path_len)) return;

    char marker[224];
    int n = snprintf(marker, sizeof(marker), "@bind:%s", path);
    if (n <= 0 || (size_t)n >= sizeof(marker)) return;
    uint16_t dep_idx = bytecode_add_dependency(c->module, marker);
    emit_op_u16(c, BC_DEP_START, dep_idx);
}

static void emit_direct_var_bind_end(Compiler *c, AstNode *expr) {
    char path[192] = {0};
    size_t path_len = 0;
    if (!build_bindable_output_path(expr, path, sizeof(path), &path_len)) return;
    emit_byte(c, BC_DEP_END);
}

static void emit_expr_dep_markers(Compiler *c, uint32_t expr_id, AstNode *expr) {
    if (!c || !expr || !expr->deps || expr->deps->count == 0) return;
    for (DepPath *p = expr->deps->paths; p; p = p->next) {
        const char *path_str = dep_path_to_string(p);
        if (!path_str || !path_str[0]) continue;
        size_t cap = strlen(path_str) + 48;
        char *marker = arena_alloc(c->arena, cap);
        int n = snprintf(marker, cap, "@exprdep:%u|%s", expr_id, path_str);
        if (n <= 0 || (size_t)n >= cap) continue;
        (void)bytecode_add_dependency(c->module, marker);
    }
}

static void emit_expr_output_wrapper_start(Compiler *c, uint32_t expr_id) {
    char id_buf[32];
    int id_n = snprintf(id_buf, sizeof(id_buf), "%u", expr_id);
    if (id_n <= 0 || (size_t)id_n >= sizeof(id_buf)) return;

    uint16_t span_tag_idx = bytecode_add_string(c->module, "span", 4);
    uint16_t attr_name_idx = bytecode_add_string(c->module, "data-mot-expr", 13);
    uint16_t id_const_idx = make_string_constant(c, id_buf, (size_t)id_n);

    emit_op_u16(c, BC_EMIT_TAG_OPEN, span_tag_idx);
    emit_op_u16(c, BC_EMIT_ATTR_START, attr_name_idx);
    emit_op_u16(c, BC_EMIT_LITERAL, id_const_idx);
    emit_byte(c, BC_EMIT_ATTR_END);
    emit_byte(c, BC_EMIT_TAG_END);
}

static void emit_expr_output_wrapper_end(Compiler *c) {
    uint16_t span_tag_idx = bytecode_add_string(c->module, "span", 4);
    emit_op_u16(c, BC_EMIT_TAG_CLOSE, span_tag_idx);
}

static bool emit_expr_bind_start(Compiler *c, AstNode *expr, uint32_t *out_expr_id) {
    if (!c || !expr || !out_expr_id || !expr->deps || expr->deps->count == 0) return false;

    char path[192] = {0};
    size_t path_len = 0;
    if (build_bindable_output_path(expr, path, sizeof(path), &path_len)) {
        return false; /* Direct bind path already handled by @bind markers. */
    }

    char *program = NULL;
    if (!build_reactive_expr_program(c, expr, &program) || !program || !program[0]) return false;

    uint32_t expr_id = ++c->reactive_expr_seq;
    size_t marker_cap = strlen(program) + 64;
    char *marker = arena_alloc(c->arena, marker_cap);
    int n = snprintf(marker, marker_cap, "@exprbind:%u|%s", expr_id, program);
    if (n <= 0 || (size_t)n >= marker_cap) return false;
    (void)bytecode_add_dependency(c->module, marker);

    emit_expr_dep_markers(c, expr_id, expr);
    emit_expr_output_wrapper_start(c, expr_id);
    *out_expr_id = expr_id;
    return true;
}

static void emit_expr_bind_end(Compiler *c, uint32_t expr_id) {
    if (!c || expr_id == 0) return;
    emit_expr_output_wrapper_end(c);
}

static bool path_matches_or_prefix(const char *a, const char *b) {
    if (!a || !b) return false;
    size_t alen = strlen(a);
    size_t blen = strlen(b);
    if (alen == blen && strcmp(a, b) == 0) return true;
    if (alen > blen &&
        strncmp(a, b, blen) == 0 &&
        a[blen] == '.') {
        return true;
    }
    if (blen > alen &&
        strncmp(b, a, alen) == 0 &&
        b[alen] == '.') {
        return true;
    }
    return false;
}

static bool add_unique_path_ptr(const char **paths, uint32_t *count, const char *path) {
    if (!paths || !count || !path || !path[0]) return false;
    for (uint32_t i = 0; i < *count; i++) {
        if (strcmp(paths[i], path) == 0) return false;
    }
    paths[*count] = path;
    (*count)++;
    return true;
}

/* Record mutable targets observed at compile time.
 * These are metadata-only markers that help map mutable paths (@set:...)
 * against rendered bindings (@bind:...). */
static void record_set_target_marker(Compiler *c, AstNode *target) {
    char path[192] = {0};
    size_t path_len = 0;
    if (!build_bindable_output_path(target, path, sizeof(path), &path_len)) return;

    char marker[224];
    int n = snprintf(marker, sizeof(marker), "@set:%s", path);
    if (n <= 0 || (size_t)n >= sizeof(marker)) return;
    (void)bytecode_add_dependency(c->module, marker);
}

/* Emit explicit reactive mapping markers @plan:<set>|<bind> so runtimes do not
 * need to infer the mapping heuristically from independent marker sets. */
static void emit_reactive_plan_markers(Compiler *c) {
    if (!c || !c->module || c->module->dep_count == 0) return;

    uint32_t dep_count = c->module->dep_count;
    const char **set_paths = arena_alloc(c->arena, sizeof(char *) * dep_count);
    const char **bind_paths = arena_alloc(c->arena, sizeof(char *) * dep_count);
    const char **bind_attr_markers = arena_alloc(c->arena, sizeof(char *) * dep_count);
    uint32_t *expr_dep_ids = arena_alloc(c->arena, sizeof(uint32_t) * dep_count);
    const char **expr_dep_paths = arena_alloc(c->arena, sizeof(char *) * dep_count);
    uint32_t set_count = 0;
    uint32_t bind_count = 0;
    uint32_t bind_attr_count = 0;
    uint32_t expr_dep_count = 0;

    for (uint32_t i = 0; i < dep_count; i++) {
        const char *dep = c->module->deps[i].path;
        if (!dep) continue;
        if (strncmp(dep, "@set:", 5) == 0 && dep[5] != '\0') {
            add_unique_path_ptr(set_paths, &set_count, dep + 5);
        } else if (strncmp(dep, "@bind:", 6) == 0 && dep[6] != '\0') {
            add_unique_path_ptr(bind_paths, &bind_count, dep + 6);
        } else if (strncmp(dep, "@bindattr:", 10) == 0 && dep[10] != '\0') {
            bind_attr_markers[bind_attr_count++] = dep + 10;
        } else if (strncmp(dep, "@exprdep:", 9) == 0 && dep[9] != '\0') {
            const char *entry = dep + 9;
            const char *sep = strchr(entry, '|');
            if (!sep || sep == entry || sep[1] == '\0') continue;
            char id_buf[32];
            size_t id_len = (size_t)(sep - entry);
            if (id_len == 0 || id_len >= sizeof(id_buf)) continue;
            memcpy(id_buf, entry, id_len);
            id_buf[id_len] = '\0';
            char *endptr = NULL;
            unsigned long parsed = strtoul(id_buf, &endptr, 10);
            if (!endptr || *endptr != '\0' || parsed > 0xFFFFFFFFUL) continue;
            expr_dep_ids[expr_dep_count] = (uint32_t)parsed;
            expr_dep_paths[expr_dep_count] = sep + 1;
            expr_dep_count++;
        }
    }

    if (set_count == 0) return;

    if (bind_count > 0) {
        for (uint32_t s = 0; s < set_count; s++) {
            const char *set_path = set_paths[s];
            for (uint32_t b = 0; b < bind_count; b++) {
                const char *bind_path = bind_paths[b];
                if (!path_matches_or_prefix(set_path, bind_path)) continue;

                char marker[512];
                int n = snprintf(marker, sizeof(marker), "@plan:%s|%s", set_path, bind_path);
                if (n <= 0 || (size_t)n >= sizeof(marker)) continue;
                (void)bytecode_add_dependency(c->module, marker);
            }
        }
    }

    if (bind_attr_count > 0) {
        for (uint32_t s = 0; s < set_count; s++) {
            const char *set_path = set_paths[s];
            for (uint32_t m = 0; m < bind_attr_count; m++) {
                const char *entry = bind_attr_markers[m];
                const char *sep1 = strchr(entry, '|');
                if (!sep1 || sep1 == entry) continue;
                const char *sep2 = strchr(sep1 + 1, '|');
                if (!sep2 || sep2 == sep1 + 1 || sep2[1] == '\0') continue;

                const char *bind_path = sep2 + 1;
                if (!path_matches_or_prefix(set_path, bind_path)) continue;

                size_t node_len = (size_t)(sep1 - entry);
                size_t attr_len = (size_t)(sep2 - (sep1 + 1));
                if (node_len == 0 || attr_len == 0) continue;

                char marker[768];
                int n = snprintf(
                    marker,
                    sizeof(marker),
                    "@planattr:%s|%.*s|%.*s|%s",
                    set_path,
                    (int)node_len, entry,
                    (int)attr_len, sep1 + 1,
                    bind_path
                );
                if (n <= 0 || (size_t)n >= sizeof(marker)) continue;
                (void)bytecode_add_dependency(c->module, marker);
            }
        }
    }

    if (expr_dep_count > 0) {
        for (uint32_t s = 0; s < set_count; s++) {
            const char *set_path = set_paths[s];
            for (uint32_t e = 0; e < expr_dep_count; e++) {
                const char *dep_path = expr_dep_paths[e];
                if (!dep_path || !dep_path[0]) continue;
                if (!path_matches_or_prefix(set_path, dep_path)) continue;

                char marker[512];
                int n = snprintf(marker, sizeof(marker), "@planexpr:%s|%u", set_path, expr_dep_ids[e]);
                if (n <= 0 || (size_t)n >= sizeof(marker)) continue;
                (void)bytecode_add_dependency(c->module, marker);
            }
        }
    }
}

/* Compile output */
static void compile_output(Compiler *c, AstNode *node) {
    c->current_line = node->line;

    if (node->data.output.expr) {
        AstNode *expr = node->data.output.expr;
        uint32_t expr_bind_id = 0;

        /* Emit dependency tracking markers if expression has dependencies */
        emit_deps_start(c, expr);
        emit_direct_var_bind_start(c, expr);
        (void)emit_expr_bind_start(c, expr, &expr_bind_id);

        compile_expr(c, expr);
        emit_byte(c, BC_EMIT_TEXT);

        emit_expr_bind_end(c, expr_bind_id);
        emit_direct_var_bind_end(c, expr);
        emit_deps_end(c, expr);
    }
}

/* Compile if statement */
static void compile_if(Compiler *c, AstNode *node) {
    c->current_line = node->line;
    int end_jumps[256];
    int end_jump_count = 0;

    /* Compile condition */
    compile_expr(c, node->data.if_stmt.condition);

    /* Jump if false to else branch */
    int else_jump = emit_jump(c, BC_JUMP_IF_FALSE);
    emit_byte(c, BC_POP);  /* Pop condition */

    /* Compile then branch */
    begin_scope(c);
    for (AstNode *child = node->data.if_stmt.then_body; child; child = child->next) {
        compile_node(c, child);
    }
    end_scope(c);

    /* Jump over else */
    end_jumps[end_jump_count++] = emit_jump(c, BC_JUMP);

    /* Patch else jump */
    patch_jump(c, else_jump);
    emit_byte(c, BC_POP);  /* Pop condition */

    /* Compile else/elsif branches.
     * Parser stores elsif chains as nested if_stmt.else_branch links. */
    for (AstNode *branch = node->data.if_stmt.else_branch; branch;) {
        AstNode *next_branch = branch->next;

        if (branch->type == NODE_ELSIF || branch->type == NODE_IF) {
            compile_expr(c, branch->data.if_stmt.condition);
            int next_jump = emit_jump(c, BC_JUMP_IF_FALSE);
            emit_byte(c, BC_POP);

            begin_scope(c);
            for (AstNode *child = branch->data.if_stmt.then_body; child; child = child->next) {
                compile_node(c, child);
            }
            end_scope(c);

            if (end_jump_count >= (int)(sizeof(end_jumps) / sizeof(end_jumps[0]))) {
                compile_error(c, branch->line, branch->column,
                              "Too many elsif branches in if statement");
                return;
            }
            end_jumps[end_jump_count++] = emit_jump(c, BC_JUMP);

            patch_jump(c, next_jump);
            emit_byte(c, BC_POP);
            next_branch = branch->data.if_stmt.else_branch ?
                          branch->data.if_stmt.else_branch : branch->next;
        } else if (branch->type == NODE_ELSE) {
            begin_scope(c);
            for (AstNode *child = branch->data.else_stmt.body; child; child = child->next) {
                compile_node(c, child);
            }
            end_scope(c);
            next_branch = NULL;  /* else is terminal */
        }

        branch = next_branch;
    }

    for (int i = 0; i < end_jump_count; i++) {
        patch_jump(c, end_jumps[i]);
    }
}

/* Compile match statement */
static void compile_match(Compiler *c, AstNode *node) {
    c->current_line = node->line;
    int end_jumps[256];
    int end_jump_count = 0;

    /* Evaluate match value once and keep it on stack through case checks */
    compile_expr(c, node->data.match.value);

    bool has_default = false;
    for (AstNode *case_node = node->data.match.cases; case_node; case_node = case_node->next) {
        if (case_node->type != NODE_CASE) continue;

        if (case_node->data.case_stmt.is_default) {
            has_default = true;
            begin_scope(c);
            for (AstNode *child = case_node->data.case_stmt.body; child; child = child->next) {
                compile_node(c, child);
            }
            end_scope(c);
            emit_byte(c, BC_POP);  /* Pop match value */
            if (end_jump_count >= (int)(sizeof(end_jumps) / sizeof(end_jumps[0]))) {
                compile_error(c, case_node->line, case_node->column,
                              "Too many match branches");
                return;
            }
            end_jumps[end_jump_count++] = emit_jump(c, BC_JUMP);
            break;  /* default is terminal */
        }

        /* Compare match value with case pattern */
        emit_byte(c, BC_DUP);
        compile_expr(c, case_node->data.case_stmt.pattern);
        emit_byte(c, BC_EQ);

        int next_case = emit_jump(c, BC_JUMP_IF_FALSE);
        emit_byte(c, BC_POP);  /* Pop comparison result (true) */

        begin_scope(c);
        for (AstNode *child = case_node->data.case_stmt.body; child; child = child->next) {
            compile_node(c, child);
        }
        end_scope(c);

        emit_byte(c, BC_POP);  /* Pop match value after a successful branch */
        if (end_jump_count >= (int)(sizeof(end_jumps) / sizeof(end_jumps[0]))) {
            compile_error(c, case_node->line, case_node->column,
                          "Too many match branches");
            return;
        }
        end_jumps[end_jump_count++] = emit_jump(c, BC_JUMP);

        patch_jump(c, next_case);
        emit_byte(c, BC_POP);  /* Pop comparison result (false) */
    }

    if (!has_default) {
        /* No match branch taken; discard the match value */
        emit_byte(c, BC_POP);
    }

    for (int i = 0; i < end_jump_count; i++) {
        patch_jump(c, end_jumps[i]);
    }
}

/* Compile for loop */
static void compile_for(Compiler *c, AstNode *node) {
    c->current_line = node->line;

    /* Compile iterable */
    compile_expr(c, node->data.for_loop.iterable);

    /* Start iteration - this pushes an iterator onto the stack */
    emit_byte(c, BC_ITER_START);

    /* Reserve a slot for the iterator (it's on the stack but not a named local) */
    c->scope->local_count++;

    int loop_start = chunk_current_offset(c->current_chunk);

    /* Iterate next (jumps to end if done, otherwise pushes current item) */
    int exit_jump = emit_jump(c, BC_ITER_NEXT);

    /* Begin scope for loop body */
    begin_scope(c);

    /* Add loop variable - ITER_NEXT pushed the item, which is now on top of stack */
    add_local(c, node->data.for_loop.item);

    /* Add index variable if present */
    if (node->data.for_loop.index) {
        /* TODO: implement index variable by pushing iteration count */
        add_local(c, node->data.for_loop.index);
    }

    /* Compile body */
    for (AstNode *child = node->data.for_loop.body; child; child = child->next) {
        compile_node(c, child);
    }

    end_scope(c);

    /* Loop back */
    emit_loop(c, loop_start);

    /* Patch exit jump */
    patch_jump(c, exit_jump);

    /* End iteration - pops the iterator */
    emit_byte(c, BC_ITER_END);

    /* Account for iterator slot being freed */
    c->scope->local_count--;
}

/* Look up a component by name */
static ComponentEntry *lookup_component(Compiler *c, const char *name) {
    for (ComponentEntry *entry = c->components; entry; entry = entry->next) {
        if (strcmp(entry->name, name) == 0) {
            return entry;
        }
    }
    return NULL;
}

/* Register a component */
static void register_component(Compiler *c, const char *name, uint16_t func_idx, AstNode *node) {
    ComponentEntry *entry = arena_alloc(c->arena, sizeof(ComponentEntry));
    entry->name = name;
    entry->func_idx = func_idx;
    entry->node = node;
    entry->next = c->components;
    c->components = entry;
}

/* Look up a dynamic import by name */
static DynamicImportEntry *lookup_dynamic_import(Compiler *c, const char *name) {
    for (DynamicImportEntry *entry = c->dynamic_imports; entry; entry = entry->next) {
        if (strcmp(entry->name, name) == 0) {
            return entry;
        }
    }
    return NULL;
}

/* Register a dynamic import */
static void register_dynamic_import(Compiler *c, const char *name, const char *path) {
    /* Add to bytecode comp_refs and get index */
    uint16_t ref_idx = bytecode_add_comp_ref(c->module, name, path);
    bool linked = false;
    if (c->linked_component_resolver) {
        linked = c->linked_component_resolver(path, c->linked_component_userdata);
    }

    DynamicImportEntry *entry = arena_alloc(c->arena, sizeof(DynamicImportEntry));
    entry->name = arena_strdup(c->arena, name);
    entry->path = arena_strdup(c->arena, path);
    entry->ref_idx = ref_idx;
    entry->linked = linked;
    entry->next = c->dynamic_imports;
    c->dynamic_imports = entry;
}

/* Look up a macro by name */
static MacroEntry *lookup_macro(Compiler *c, const char *name) {
    for (MacroEntry *entry = c->macros; entry; entry = entry->next) {
        if (strcmp(entry->name, name) == 0) {
            return entry;
        }
    }
    return NULL;
}

/* Register a macro */
static void register_macro(Compiler *c, const char *name, AstNode *node) {
    MacroEntry *entry = arena_alloc(c->arena, sizeof(MacroEntry));
    entry->name = name;
    entry->node = node;
    entry->next = c->macros;
    c->macros = entry;
}

static AstNode *find_attr(AstNode *attrs, const char *name) {
    for (AstNode *attr = attrs; attr; attr = attr->next) {
        if (attr->type == NODE_ATTR && strcmp(attr->data.attr.name, name) == 0) {
            return attr;
        }
    }
    return NULL;
}

/* Compile macro invocation */
static void compile_macro_call(Compiler *c, AstNode *invocation, MacroEntry *macro) {
    AstNode *macro_node = macro->node;

    begin_scope(c);

    /* Bind macro parameters from invocation attributes. */
    for (AstNode *param = macro_node->data.macro.params; param; param = param->next) {
        if (param->type != NODE_IDENT) continue;
        const char *param_name = param->data.ident.name;
        AstNode *arg = find_attr(invocation->data.element.attrs, param_name);

        if (arg && arg->data.attr.value) {
            compile_expr(c, arg->data.attr.value);
        } else if (arg) {
            /* Shorthand argument: <macroName paramName> == paramName=paramName */
            int slot = resolve_local(c, param_name);
            if (slot >= 0) {
                emit_op_u16(c, BC_LOAD, (uint16_t)slot);
            } else {
                compile_error(c, invocation->line, invocation->column,
                              "Undefined macro argument '%s'", param_name);
                emit_byte(c, BC_NULL);
            }
        } else {
            emit_byte(c, BC_NULL);
        }

        add_local(c, param_name);
    }

    bool prev_in_macro = c->in_macro;
    AstNode *prev_macro_children = c->macro_children;
    c->in_macro = true;
    c->macro_children = invocation->data.element.children;

    for (AstNode *child = macro_node->data.macro.body; child; child = child->next) {
        compile_node(c, child);
    }

    c->in_macro = prev_in_macro;
    c->macro_children = prev_macro_children;
    end_scope(c);
}

/* Compile component definition */
static void compile_defcomp(Compiler *c, AstNode *node) {
    c->current_line = node->line;

    const char *name = node->data.defcomp.name;

    /* Check for redefinition */
    if (lookup_component(c, name)) {
        compile_error(c, node->line, node->column,
                      "Component '%s' already defined", name);
        return;
    }

    /* Create a new function chunk for the component body */
    uint16_t func_idx = bytecode_add_function(c->module);
    Chunk *comp_chunk = bytecode_get_function(c->module, func_idx);
    bytecode_set_function_debug(c->module, func_idx, name, node->source_path);

    /* Save current compilation state */
    Chunk *prev_chunk = c->current_chunk;
    CompilerScope *prev_scope = c->scope;
    int prev_depth = c->scope_depth;
    bool prev_in_component = c->in_component;

    if (c->partial_eval && c->pe) {
        pe_push_scope(c->pe);
    }

    /* Set up component compilation context */
    c->current_chunk = comp_chunk;
    c->scope = arena_alloc(c->arena, sizeof(CompilerScope));
    c->scope->enclosing = NULL;
    c->scope->locals = NULL;
    c->scope->local_count = 0;
    c->scope->scope_depth = 0;
    c->scope_depth = 0;
    c->in_component = true;

    /* Add 'props' as a local (slot 0) - passed as first argument */
    add_local(c, "__props__");

    /* Add 'children' as a local (slot 1) - passed as second argument */
    add_local(c, "__children__");

    /* Define props as locals from __props__ */
    for (AstNode *prop = node->data.defcomp.props; prop; prop = prop->next) {
        if (prop->type == NODE_PROP_DEF) {
            const char *prop_name = prop->data.prop_def.name;

            /* Load from props object */
            emit_op_u16(c, BC_LOAD, 0);  /* Load __props__ */
            uint16_t name_idx = bytecode_add_string(c->module, prop_name,
                                                    (uint32_t)strlen(prop_name));
            emit_op_u16(c, BC_LOAD_FIELD, name_idx);

            /* If prop has default and value is null, use default */
            if (prop->data.prop_def.default_val) {
                int skip_default = emit_jump(c, BC_JUMP_IF_TRUE);
                emit_byte(c, BC_POP);
                compile_expr(c, prop->data.prop_def.default_val);
                patch_jump(c, skip_default);
            }

            add_local(c, prop_name);
        }
    }

    /* Compile component body */
    for (AstNode *child = node->data.defcomp.body; child; child = child->next) {
        compile_node(c, child);
    }

    /* Return from component */
    emit_byte(c, BC_RETURN);

    /* Restore previous compilation state */
    c->current_chunk = prev_chunk;
    c->scope = prev_scope;
    c->scope_depth = prev_depth;
    c->in_component = prev_in_component;

    if (c->partial_eval && c->pe) {
        pe_pop_scope(c->pe);
    }

    /* Register the component */
    register_component(c, name, func_idx, node);
}

/* Compile children placeholder */
static void compile_children(Compiler *c, AstNode *node) {
    c->current_line = node->line;

    if (c->in_component) {
        int children_slot = resolve_local(c, "__children__");
        if (children_slot < 0) {
            compile_error(c, node->line, node->column,
                          "Internal error: missing __children__ slot in component");
            return;
        }
        emit_op_u16(c, BC_LOAD, (uint16_t)children_slot);
        /* Emit BC_SLOT_DEFAULT - render unnamed children passed to this component. */
        emit_byte(c, BC_SLOT_DEFAULT);
        return;
    }

    if (c->in_macro) {
        for (AstNode *child = c->macro_children; child; child = child->next) {
            compile_node(c, child);
        }
        return;
    }

    if (!c->in_component && !c->in_macro) {
        compile_error(c, node->line, node->column,
                      "<children> can only be used inside a component or macro");
        return;
    }
}

/* Compile props object from element attributes */
static void compile_props_object(Compiler *c, AstNode *node) {
    int prop_count = 0;
    for (AstNode *attr = node->data.element.attrs; attr; attr = attr->next) {
        if (attr->type == NODE_ATTR) {
            prop_count++;
        }
    }

    emit_op_u16(c, BC_OBJECT_NEW, (uint16_t)prop_count);

    for (AstNode *attr = node->data.element.attrs; attr; attr = attr->next) {
        if (attr->type == NODE_ATTR) {
            uint16_t key_idx = bytecode_add_string(c->module,
                attr->data.attr.name, (uint32_t)strlen(attr->data.attr.name));
            if (attr->data.attr.value) {
                compile_expr(c, attr->data.attr.value);
            } else {
                emit_byte(c, BC_TRUE);
            }
            emit_op_u16(c, BC_OBJECT_SET, key_idx);
        }
    }
}

/* Compile component instantiation (inline component) */
static void compile_component_call(Compiler *c, AstNode *node, ComponentEntry *comp) {
    c->current_line = node->line;

    /* Capture children first so any locals declared inside children are not
     * misaligned by transient props stack values. */
    if (node->data.element.children) {
        emit_op_u16(c, BC_SLOT_START, 0);
        for (AstNode *child = node->data.element.children; child; child = child->next) {
            compile_node(c, child);
        }
        emit_byte(c, BC_SLOT_END);
    } else {
        emit_byte(c, BC_NULL);
    }

    begin_scope(c);
    uint16_t children_slot = add_local(c, "<tmp_children>");

    /* Build props after child capture, then reload children to preserve call order:
     * stack before BC_COMPONENT_START: [props, children]. */
    compile_props_object(c, node);
    emit_op_u16(c, BC_LOAD, children_slot);

    /* Emit component call */
    emit_byte(c, BC_COMPONENT_START);
    emit_u16(c, comp->func_idx);

    /* End component */
    emit_byte(c, BC_COMPONENT_END);
    end_scope(c);
}

/* Compile dynamic component call (component loaded at edge) */
static void compile_dynamic_component_call(Compiler *c, AstNode *node, DynamicImportEntry *dyn) {
    c->current_line = node->line;

    /* Capture children first so any locals declared inside children are not
     * misaligned by transient props stack values. */
    if (node->data.element.children) {
        emit_op_u16(c, BC_SLOT_START, 0);
        for (AstNode *child = node->data.element.children; child; child = child->next) {
            compile_node(c, child);
        }
        emit_byte(c, BC_SLOT_END);
    } else {
        emit_byte(c, BC_NULL);
    }

    begin_scope(c);
    uint16_t children_slot = add_local(c, "<tmp_children>");

    /* Build props after child capture, then reload children to preserve call order:
     * stack before component load call: [props, children]. */
    compile_props_object(c, node);
    emit_op_u16(c, BC_LOAD, children_slot);

    /* Linked imports are resolved inside the runtime; non-linked imports are
     * loaded on demand from host/origin. */
    emit_byte(c, dyn->linked ? BC_COMPONENT_LINKED : BC_COMPONENT_LOAD);
    emit_u16(c, dyn->ref_idx);
    emit_byte(c, 2);  /* argc: props + children */

    /* Component execution happens at edge */
    emit_byte(c, BC_COMPONENT_END);
    end_scope(c);
}

static void emit_expr_attr_marker(Compiler *c, uint32_t node_id, const char *attr_name, AstNode *expr) {
    if (!c || node_id == 0 || !attr_name || !expr || !expr->deps || expr->deps->count == 0) return;

    char path[192] = {0};
    size_t path_len = 0;
    if (build_bindable_output_path(expr, path, sizeof(path), &path_len)) {
        return; /* Direct attr bind markers already cover this path. */
    }

    char *program = NULL;
    if (!build_reactive_expr_program(c, expr, &program) || !program || !program[0]) return;

    uint32_t expr_id = ++c->reactive_expr_seq;
    size_t marker_cap = strlen(attr_name) + strlen(program) + 96;
    char *marker = arena_alloc(c->arena, marker_cap);
    int n = snprintf(marker, marker_cap, "@exprattr:%u|%s|%u|%s", node_id, attr_name, expr_id, program);
    if (n <= 0 || (size_t)n >= marker_cap) return;
    (void)bytecode_add_dependency(c->module, marker);
    emit_expr_dep_markers(c, expr_id, expr);
}

/* Compile HTML element */
static void compile_element(Compiler *c, AstNode *node) {
    c->current_line = node->line;

    const char *tag = node->data.element.tag;

    /* Compile-time macro expansion. */
    MacroEntry *macro = lookup_macro(c, tag);
    if (macro) {
        compile_macro_call(c, node, macro);
        return;
    }

    /* Check if this is a component call (uppercase first letter = component) */
    if (tag[0] >= 'A' && tag[0] <= 'Z') {
        /* First check for inline (static) component */
        ComponentEntry *comp = lookup_component(c, tag);
        if (comp) {
            compile_component_call(c, node, comp);
            return;
        }

        /* Check for dynamic import (component loaded at edge) */
        DynamicImportEntry *dyn = lookup_dynamic_import(c, tag);
        if (dyn) {
            compile_dynamic_component_call(c, node, dyn);
            return;
        }
        /* Not a registered component - treat as regular element */
    }

    uint16_t tag_idx = bytecode_add_string(c->module, tag, (uint32_t)strlen(tag));
    uint32_t reactive_node_id = 0;

    /* If this element has reactive attribute expressions, assign a stable node id
     * so browser runtime can patch attributes without full rerender. */
    for (AstNode *attr = node->data.element.attrs; attr; attr = attr->next) {
        if (attr->type != NODE_ATTR || !attr->data.attr.value) continue;
        AstNode *value_expr = attr->data.attr.value;
        if (!value_expr->deps || value_expr->deps->count == 0) {
            continue;
        }
        char bind_path[192] = {0};
        size_t bind_len = 0;
        bool direct_bindable = build_bindable_output_path(value_expr, bind_path, sizeof(bind_path), &bind_len);
        if (!direct_bindable && !reactive_expr_is_supported(value_expr)) continue;
        reactive_node_id = ++c->reactive_node_seq;
        break;
    }

    /* Open tag: emit <tagname (without >) */
    emit_op_u16(c, BC_EMIT_TAG_OPEN, tag_idx);

    if (reactive_node_id != 0) {
        char id_buf[32];
        int id_n = snprintf(id_buf, sizeof(id_buf), "%u", reactive_node_id);
        if (id_n > 0 && (size_t)id_n < sizeof(id_buf)) {
            uint16_t node_attr_name_idx = bytecode_add_string(c->module, "data-mot-node", 13);
            emit_op_u16(c, BC_EMIT_ATTR_START, node_attr_name_idx);
            uint16_t id_const_idx = make_string_constant(c, id_buf, (size_t)id_n);
            emit_op_u16(c, BC_EMIT_LITERAL, id_const_idx);
            emit_byte(c, BC_EMIT_ATTR_END);
        }
    }

    /* Emit attributes */
    for (AstNode *attr = node->data.element.attrs; attr; attr = attr->next) {
        if (attr->type == NODE_ATTR) {
            uint16_t name_idx = bytecode_add_string(c->module,
                attr->data.attr.name, (uint32_t)strlen(attr->data.attr.name));
            emit_op_u16(c, BC_EMIT_ATTR_START, name_idx);
            if (attr->data.attr.value) {
                if (reactive_node_id != 0) {
                    char bind_path[192] = {0};
                    size_t bind_len = 0;
                    if (build_bindable_output_path(attr->data.attr.value, bind_path, sizeof(bind_path), &bind_len)) {
                        char marker[512];
                        int n = snprintf(
                            marker, sizeof(marker),
                            "@bindattr:%u|%s|%s",
                            reactive_node_id,
                            attr->data.attr.name,
                            bind_path
                        );
                        if (n > 0 && (size_t)n < sizeof(marker)) {
                            (void)bytecode_add_dependency(c->module, marker);
                        }
                    } else {
                        emit_expr_attr_marker(c, reactive_node_id, attr->data.attr.name, attr->data.attr.value);
                    }
                }
                compile_expr(c, attr->data.attr.value);
                emit_byte(c, BC_EMIT_TEXT);
            }
            emit_byte(c, BC_EMIT_ATTR_END);
        }
    }

    /* Pre-scan children for <on> event handlers and emit data attributes */
    for (AstNode *child = node->data.element.children; child; child = child->next) {
        if (child->type != NODE_ON) continue;

        const char *event = child->data.on_handler.event;
        char *action_program = NULL;
        if (!build_on_action_program(c, child->data.on_handler.body, &action_program)) {
            compile_error(c, child->line, child->column,
                          "Cannot serialize <on %s> action program", event);
            continue;
        }

        /* Emit data-mot-on-{event} attribute */
        char attr_name[64];
        int an = snprintf(attr_name, sizeof(attr_name), "data-mot-on-%s", event);
        if (an <= 0 || (size_t)an >= sizeof(attr_name)) continue;

        uint16_t attr_name_idx = bytecode_add_string(c->module, attr_name, (uint32_t)an);
        emit_op_u16(c, BC_EMIT_ATTR_START, attr_name_idx);

        uint16_t prog_const = make_string_constant(c, action_program, strlen(action_program));
        emit_op_u16(c, BC_EMIT_LITERAL, prog_const);

        emit_byte(c, BC_EMIT_ATTR_END);

        /* Record @set markers so reactive plan linkage works */
        for (AstNode *stmt = child->data.on_handler.body; stmt; stmt = stmt->next) {
            if (stmt->type == NODE_SET) {
                record_set_target_marker(c, stmt->data.set.target);
            }
        }
    }

    /* Self-closing? */
    if (node->data.element.self_closing) {
        emit_byte(c, BC_EMIT_TAG_SELF);  /* Emit /> */
        return;
    }

    /* Close opening tag with > */
    emit_byte(c, BC_EMIT_TAG_END);

    /* Compile children (skip <on> handlers, already emitted as data attributes) */
    for (AstNode *child = node->data.element.children; child; child = child->next) {
        if (child->type == NODE_ON) continue;
        compile_node(c, child);
    }

    /* Close tag */
    emit_op_u16(c, BC_EMIT_TAG_CLOSE, tag_idx);
}

/* Compile text node */
static void compile_text(Compiler *c, AstNode *node) {
    c->current_line = node->line;

    uint16_t idx = make_string_constant(c, node->data.text.content, node->data.text.length);
    emit_op_u16(c, BC_EMIT_LITERAL, idx);
}

/* Compile embedded style/script */
static void compile_embedded(Compiler *c, AstNode *node) {
    c->current_line = node->line;

    /* Emit opening tag - use bytecode_add_string for tag names (not make_string_constant) */
    const char *tag = node->type == NODE_STYLE ? "style" : "script";
    uint16_t tag_idx = bytecode_add_string(c->module, tag, (uint32_t)strlen(tag));
    emit_op_u16(c, BC_EMIT_TAG_OPEN, tag_idx);
    emit_byte(c, BC_EMIT_TAG_END);  /* Close the opening tag with > */

    /* Emit content as literal - use make_string_constant for BC_EMIT_LITERAL */
    if (node->data.embedded.code && node->data.embedded.code_len > 0) {
        uint16_t content_idx = make_string_constant(c, node->data.embedded.code, node->data.embedded.code_len);
        emit_op_u16(c, BC_EMIT_LITERAL, content_idx);
    }

    /* Emit closing tag */
    emit_op_u16(c, BC_EMIT_TAG_CLOSE, tag_idx);
}

/* Collect bound input field names from mutation body */
static void collect_bound_fields(AstNode *body, BytecodeModule *mod, uint16_t mut_idx) {
    for (AstNode *child = body; child; child = child->next) {
        if (child->type == NODE_BOUND_INPUT) {
            bytecode_mutation_req_add_field(mod, mut_idx, child->data.bound_input.field_name);
        }
    }
}

/* Compile <insert into binding> ... </insert> */
static void compile_insert(Compiler *c, AstNode *node) {
    c->current_line = node->line;

    uint16_t mut_idx = bytecode_add_mutation_req(c->module, MUTATE_INSERT, node->data.insert.target, node->data.insert.optimistic);
    collect_bound_fields(node->data.insert.body, c->module, mut_idx);

    emit_op_u16(c, BC_MUTATE_START, mut_idx);
    for (AstNode *child = node->data.insert.body; child; child = child->next) {
        compile_node(c, child);
    }
    emit_byte(c, BC_MUTATE_END);
}

/* Compile <update binding where cond> ... </update> */
static void compile_update(Compiler *c, AstNode *node) {
    c->current_line = node->line;

    uint16_t mut_idx = bytecode_add_mutation_req(c->module, MUTATE_UPDATE, node->data.update.target, node->data.update.optimistic);
    collect_bound_fields(node->data.update.body, c->module, mut_idx);

    emit_op_u16(c, BC_MUTATE_START, mut_idx);
    for (AstNode *child = node->data.update.body; child; child = child->next) {
        compile_node(c, child);
    }
    emit_byte(c, BC_MUTATE_END);
}

/* Compile <delete from binding where cond> ... </delete> */
static void compile_delete(Compiler *c, AstNode *node) {
    c->current_line = node->line;

    uint16_t mut_idx = bytecode_add_mutation_req(c->module, MUTATE_DELETE, node->data.delete_stmt.target, node->data.delete_stmt.optimistic);
    collect_bound_fields(node->data.delete_stmt.body, c->module, mut_idx);

    emit_op_u16(c, BC_MUTATE_START, mut_idx);
    for (AstNode *child = node->data.delete_stmt.body; child; child = child->next) {
        compile_node(c, child);
    }
    emit_byte(c, BC_MUTATE_END);
}

/* Compile <input obj.field /> inside mutation block */
static void compile_bound_input(Compiler *c, AstNode *node) {
    c->current_line = node->line;

    uint16_t input_tag = bytecode_add_string(c->module, "input", 5);
    uint16_t type_attr = bytecode_add_string(c->module, "type", 4);
    uint16_t name_attr = bytecode_add_string(c->module, "name", 4);
    uint16_t value_attr = bytecode_add_string(c->module, "value", 5);
    uint16_t type_val = make_string_constant(c, "text", 4);
    uint16_t name_val = make_string_constant(c, node->data.bound_input.field_name,
                                              strlen(node->data.bound_input.field_name));

    emit_op_u16(c, BC_EMIT_TAG_OPEN, input_tag);

    /* type="text" */
    emit_op_u16(c, BC_EMIT_ATTR_START, type_attr);
    emit_op_u16(c, BC_EMIT_LITERAL, type_val);
    emit_byte(c, BC_EMIT_ATTR_END);

    /* name="field_name" */
    emit_op_u16(c, BC_EMIT_ATTR_START, name_attr);
    emit_op_u16(c, BC_EMIT_LITERAL, name_val);
    emit_byte(c, BC_EMIT_ATTR_END);

    /* value=obj.field (load from scope) */
    int obj_slot = resolve_local(c, node->data.bound_input.object_name);
    if (obj_slot >= 0) {
        emit_op_u16(c, BC_EMIT_ATTR_START, value_attr);
        emit_op_u16(c, BC_LOAD, (uint16_t)obj_slot);
        uint16_t field_idx = bytecode_add_string(c->module, node->data.bound_input.field_name,
                                                  (uint32_t)strlen(node->data.bound_input.field_name));
        emit_op_u16(c, BC_LOAD_FIELD, field_idx);
        emit_byte(c, BC_EMIT_TEXT);
        emit_byte(c, BC_EMIT_ATTR_END);
    }

    /* Extra HTML attributes */
    for (AstNode *attr = node->data.bound_input.attrs; attr; attr = attr->next) {
        if (attr->type != NODE_ATTR) continue;
        uint16_t attr_name_idx = bytecode_add_string(c->module, attr->data.attr.name,
                                                      (uint32_t)strlen(attr->data.attr.name));
        if (attr->data.attr.value) {
            emit_op_u16(c, BC_EMIT_ATTR_START, attr_name_idx);
            compile_expr(c, attr->data.attr.value);
            emit_byte(c, BC_EMIT_TEXT);
            emit_byte(c, BC_EMIT_ATTR_END);
        } else {
            /* Boolean attribute (e.g., required) */
            emit_op_u16(c, BC_EMIT_ATTR_START, attr_name_idx);
            emit_byte(c, BC_EMIT_ATTR_END);
        }
    }

    emit_byte(c, BC_EMIT_TAG_SELF);
}

/* Compile node dispatch */
static void compile_node(Compiler *c, AstNode *node) {
    if (!node) return;

    switch (node->type) {
        case NODE_LET:
            compile_let(c, node);
            break;
        case NODE_VAR:
            compile_var(c, node);
            break;
        case NODE_SET:
            compile_set(c, node);
            break;
        case NODE_OUTPUT:
            compile_output(c, node);
            break;
        case NODE_IF:
            compile_if(c, node);
            break;
        case NODE_MATCH:
            compile_match(c, node);
            break;
        case NODE_FOR:
            compile_for(c, node);
            break;
        case NODE_ELEMENT:
            compile_element(c, node);
            break;
        case NODE_TEXT:
            compile_text(c, node);
            break;
        case NODE_COMMENT:
            /* Skip comments */
            break;
        case NODE_STYLE:
        case NODE_SCRIPT:
            compile_embedded(c, node);
            break;
        case NODE_DEFCOMP:
            compile_defcomp(c, node);
            break;
        case NODE_INTERFACE:
            /* Interface declarations are compile-time metadata only. */
            break;
        case NODE_EXPORT:
            /* Export declarations affect symbol metadata only. */
            break;
        case NODE_MACRO:
            if (lookup_macro(c, node->data.macro.name)) {
                compile_error(c, node->line, node->column,
                              "Macro '%s' already defined", node->data.macro.name);
                break;
            }
            register_macro(c, node->data.macro.name, node);
            break;
        case NODE_CHILDREN:
            compile_children(c, node);
            break;
        case NODE_IMPORT: {
            /* Register every import as a dynamic/linked component reference.
             * Static "inline" import expansion is not implemented: the linker
             * resolves bundled (linked) components at runtime, and remote ones
             * are loaded via host RPC.  Treat imports uniformly so that
             * <Foo /> tag compilation can locate them. */
            const char *path = node->data.import.from_path;
            for (AstNode *name = node->data.import.names; name; name = name->next) {
                if (name->type == NODE_IDENT) {
                    register_dynamic_import(c, name->data.ident.name, path);
                }
            }
            break;
        }
        case NODE_INSERT:
            compile_insert(c, node);
            break;
        case NODE_UPDATE:
            compile_update(c, node);
            break;
        case NODE_DELETE:
            compile_delete(c, node);
            break;
        case NODE_BOUND_INPUT:
            compile_bound_input(c, node);
            break;
        case NODE_ON:
            /* <on> handlers are compiled as data attributes on the parent element.
             * Reaching here means <on> was used outside an element context. */
            compile_error(c, node->line, node->column,
                          "<on> must be a direct child of an HTML element");
            break;
        case NODE_REQUIRE_AUTH:
            /* Set auth flags in bytecode module */
            c->module->flags |= BYTECODE_FLAG_AUTH_REQUIRED;
            if (node->data.require_auth.role &&
                strcmp(node->data.require_auth.role, "admin") == 0) {
                c->module->flags |= BYTECODE_FLAG_ADMIN_REQUIRED;
            }
            break;
        default:
            compile_error(c, node->line, node->column,
                          "Cannot compile node type %d", node->type);
    }
}

/* Register builtin functions */
static void register_builtins(Compiler *c) {
    /* String functions */
    bytecode_add_builtin(c->module, "uppercase", 1, 1);
    bytecode_add_builtin(c->module, "lowercase", 1, 1);
    bytecode_add_builtin(c->module, "trim", 1, 1);
    bytecode_add_builtin(c->module, "length", 1, 1);
    bytecode_add_builtin(c->module, "concat", 2, 255);
    bytecode_add_builtin(c->module, "substring", 2, 3);
    bytecode_add_builtin(c->module, "split", 2, 2);
    bytecode_add_builtin(c->module, "join", 2, 2);
    bytecode_add_builtin(c->module, "replace", 3, 3);

    /* Numeric functions */
    bytecode_add_builtin(c->module, "abs", 1, 1);
    bytecode_add_builtin(c->module, "round", 1, 2);
    bytecode_add_builtin(c->module, "floor", 1, 1);
    bytecode_add_builtin(c->module, "ceil", 1, 1);
    bytecode_add_builtin(c->module, "min", 2, 255);
    bytecode_add_builtin(c->module, "max", 2, 255);

    /* Array functions */
    bytecode_add_builtin(c->module, "first", 1, 1);
    bytecode_add_builtin(c->module, "last", 1, 1);
    bytecode_add_builtin(c->module, "reverse", 1, 1);
    bytecode_add_builtin(c->module, "sort", 1, 2);
    bytecode_add_builtin(c->module, "filter", 2, 2);
    bytecode_add_builtin(c->module, "map", 2, 2);
    bytecode_add_builtin(c->module, "find", 2, 2);
    bytecode_add_builtin(c->module, "includes", 2, 2);

    /* Date/time */
    bytecode_add_builtin(c->module, "now", 0, 0);
    bytecode_add_builtin(c->module, "formatDate", 2, 2);
    bytecode_add_builtin(c->module, "parseDate", 2, 2);

    /* Type checking */
    bytecode_add_builtin(c->module, "typeof", 1, 1);
    bytecode_add_builtin(c->module, "defined", 1, 1);

    /* JSON */
    bytecode_add_builtin(c->module, "json", 1, 1);
    bytecode_add_builtin(c->module, "parseJson", 1, 1);

    /* HTML */
    bytecode_add_builtin(c->module, "escape", 1, 1);
    bytecode_add_builtin(c->module, "raw", 1, 1);
}

Compiler *compiler_new(Arena *arena, Analyzer *analyzer) {
    Compiler *c = arena_alloc(arena, sizeof(Compiler));

    c->arena = arena;
    c->module = bytecode_module_new(arena);
    c->current_chunk = &c->module->main;

    c->scope = arena_alloc(arena, sizeof(CompilerScope));
    c->scope->enclosing = NULL;
    c->scope->locals = NULL;
    c->scope->local_count = 0;
    c->scope->scope_depth = 0;
    c->scope->loop_start = -1;
    c->scope->loop_end_jump = -1;

    c->scope_depth = 0;
    c->analyzer = analyzer;
    c->components = NULL;
    c->macros = NULL;
    c->dynamic_imports = NULL;
    c->current_component_slot = 0;
    c->in_component = false;
    c->in_macro = false;
    c->macro_children = NULL;

    c->had_error = false;
    c->error_msg = NULL;
    c->error_line = 0;
    c->error_col = 0;
    c->current_line = 1;
    c->reactive_node_seq = 0;
    c->reactive_expr_seq = 0;

    c->partial_eval = true;
    c->pe = c->partial_eval ? pe_new(arena, analyzer) : NULL;
    c->linked_component_resolver = NULL;
    c->linked_component_userdata = NULL;

    /* Register builtins */
    register_builtins(c);

    return c;
}

BytecodeModule *compiler_compile(Compiler *c, AstNode *doc) {
    if (!doc || doc->type != NODE_DOCUMENT) {
        compile_error(c, 0, 0, "Expected document node");
        return NULL;
    }

    if (c->partial_eval) {
        /* Start from a clean partial-eval scope for each compile invocation. */
        c->pe = pe_new(c->arena, c->analyzer);
    }

    c->reactive_node_seq = 0;
    c->reactive_expr_seq = 0;

    /* Compile all children */
    for (AstNode *child = doc->data.document.children; child; child = child->next) {
        compile_node(c, child);
    }

    emit_reactive_plan_markers(c);

    /* Emit halt */
    emit_byte(c, BC_HALT);

    return c->had_error ? NULL : c->module;
}

bool compiler_ok(Compiler *c) {
    return !c->had_error;
}

const char *compiler_error(Compiler *c) {
    return c->error_msg;
}

void compiler_set_partial_eval(Compiler *c, bool enabled) {
    c->partial_eval = enabled;
    if (enabled && !c->pe) {
        c->pe = pe_new(c->arena, c->analyzer);
    }
}

void compiler_set_linked_component_resolver(
    Compiler *c,
    CompilerLinkedComponentResolverFn resolver,
    void *userdata
) {
    if (!c) return;
    c->linked_component_resolver = resolver;
    c->linked_component_userdata = userdata;
}
