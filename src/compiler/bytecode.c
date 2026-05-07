/*
 * Bytecode implementation
 */

#include "bytecode.h"
#ifndef MOT_WASM_FREESTANDING
#include <stdio.h>
#endif
#include <string.h>

#define INITIAL_CHUNK_SIZE 256
#define INITIAL_CONST_SIZE 64
#define INITIAL_STRING_SIZE 64

/* Grow array helper */
static void *grow_array(Arena *arena, void *old, size_t old_size, size_t new_size, size_t elem_size) {
    void *new_ptr = arena_alloc(arena, new_size * elem_size);
    if (old && old_size > 0) {
        memcpy(new_ptr, old, old_size * elem_size);
    }
    return new_ptr;
}

static uint32_t fnv1a_update(uint32_t hash, const void *data, size_t len) {
    const uint8_t *bytes = (const uint8_t *)data;
    for (size_t i = 0; i < len; i++) {
        hash ^= bytes[i];
        hash *= 16777619u;
    }
    return hash;
}

static uint32_t data_req_signature(const char *query, uint32_t query_len,
                                   uint16_t param_count, const char **param_names) {
    uint32_t hash = 2166136261u;
    hash = fnv1a_update(hash, query, query_len);
    hash = fnv1a_update(hash, &param_count, sizeof(param_count));

    for (uint16_t i = 0; i < param_count; i++) {
        uint16_t name_len = (uint16_t)strlen(param_names[i]);
        hash = fnv1a_update(hash, &name_len, sizeof(name_len));
        hash = fnv1a_update(hash, param_names[i], name_len);
    }

    return hash;
}

BytecodeModule *bytecode_module_new(Arena *arena) {
    BytecodeModule *mod = arena_alloc(arena, sizeof(BytecodeModule));

    mod->magic = BYTECODE_MAGIC;
    mod->version_major = BYTECODE_VERSION_MAJOR;
    mod->version_minor = BYTECODE_VERSION_MINOR;
    mod->flags = 0;

    mod->constants = NULL;
    mod->const_count = 0;
    mod->const_cap = 0;

    mod->strings = NULL;
    mod->string_count = 0;
    mod->string_cap = 0;

    mod->data_reqs = NULL;
    mod->data_req_count = 0;
    mod->data_req_cap = 0;

    mod->deps = NULL;
    mod->dep_count = 0;
    mod->dep_cap = 0;

    mod->builtins = NULL;
    mod->builtin_count = 0;
    mod->builtin_cap = 0;

    mod->comp_refs = NULL;
    mod->comp_ref_count = 0;
    mod->comp_ref_cap = 0;

    chunk_init(&mod->main, arena);

    mod->functions = NULL;
    mod->func_count = 0;
    mod->func_cap = 0;
    mod->func_debug = NULL;
    mod->func_debug_cap = 0;
    mod->debug_spans = NULL;
    mod->debug_span_count = 0;

    mod->arena = arena;

    return mod;
}

void chunk_init(Chunk *chunk, Arena *arena) {
    chunk->code = arena_alloc(arena, INITIAL_CHUNK_SIZE);
    chunk->code_len = 0;
    chunk->code_cap = INITIAL_CHUNK_SIZE;

    chunk->lines = arena_alloc(arena, INITIAL_CHUNK_SIZE * sizeof(uint32_t));
    chunk->lines_len = 0;
    chunk->lines_cap = INITIAL_CHUNK_SIZE;
}

void chunk_write(Chunk *chunk, uint8_t byte, uint32_t line, Arena *arena) {
    if (chunk->code_len >= chunk->code_cap) {
        uint32_t new_cap = chunk->code_cap * 2;
        chunk->code = grow_array(arena, chunk->code, chunk->code_cap, new_cap, 1);
        chunk->code_cap = new_cap;
    }
    if (chunk->lines_len >= chunk->lines_cap) {
        uint32_t new_cap = chunk->lines_cap * 2;
        chunk->lines = grow_array(arena, chunk->lines, chunk->lines_cap, new_cap, sizeof(uint32_t));
        chunk->lines_cap = new_cap;
    }

    chunk->code[chunk->code_len] = byte;
    chunk->lines[chunk->lines_len] = line;
    chunk->code_len++;
    chunk->lines_len++;
}

void chunk_write_u16(Chunk *chunk, uint16_t value, uint32_t line, Arena *arena) {
    chunk_write(chunk, value & 0xFF, line, arena);          /* little-endian: low byte first */
    chunk_write(chunk, (value >> 8) & 0xFF, line, arena);
}

void chunk_write_i16(Chunk *chunk, int16_t value, uint32_t line, Arena *arena) {
    chunk_write_u16(chunk, (uint16_t)value, line, arena);
}

uint32_t chunk_current_offset(Chunk *chunk) {
    return chunk->code_len;
}

void chunk_patch_jump(Chunk *chunk, uint32_t offset, int16_t jump) {
    chunk->code[offset] = jump & 0xFF;                      /* little-endian: low byte first */
    chunk->code[offset + 1] = (jump >> 8) & 0xFF;
}

uint16_t bytecode_add_constant(BytecodeModule *mod, Constant constant) {
    if (mod->const_count >= mod->const_cap) {
        uint32_t new_cap = mod->const_cap == 0 ? INITIAL_CONST_SIZE : mod->const_cap * 2;
        mod->constants = grow_array(mod->arena, mod->constants, mod->const_cap,
                                    new_cap, sizeof(Constant));
        mod->const_cap = new_cap;
    }

    mod->constants[mod->const_count] = constant;
    return (uint16_t)mod->const_count++;
}

uint16_t bytecode_add_string(BytecodeModule *mod, const char *str, uint32_t len) {
    /* Check for existing string */
    for (uint32_t i = 0; i < mod->string_count; i++) {
        if (strlen(mod->strings[i]) == len && memcmp(mod->strings[i], str, len) == 0) {
            return (uint16_t)i;
        }
    }

    if (mod->string_count >= mod->string_cap) {
        uint32_t new_cap = mod->string_cap == 0 ? INITIAL_STRING_SIZE : mod->string_cap * 2;
        mod->strings = grow_array(mod->arena, mod->strings, mod->string_cap,
                                  new_cap, sizeof(char*));
        mod->string_cap = new_cap;
    }

    char *copy = arena_alloc(mod->arena, len + 1);
    memcpy(copy, str, len);
    copy[len] = '\0';

    mod->strings[mod->string_count] = copy;
    return (uint16_t)mod->string_count++;
}

uint16_t bytecode_add_number(BytecodeModule *mod, double value) {
    Constant c = { .type = CONST_NUMBER, .v.number = value };
    return bytecode_add_constant(mod, c);
}

uint16_t bytecode_add_int(BytecodeModule *mod, int64_t value) {
    Constant c = { .type = CONST_INT, .v.integer = value };
    return bytecode_add_constant(mod, c);
}

uint16_t bytecode_add_data_req(BytecodeModule *mod, const char *name,
                               const char *query, uint32_t query_len,
                               bool is_single, bool is_dynamic,
                               uint32_t line, uint32_t column,
                               uint16_t param_count,
                               const char **param_names,
                               const uint16_t *param_slots) {
    if (mod->data_req_count >= mod->data_req_cap) {
        uint32_t new_cap = mod->data_req_cap == 0 ? 16 : mod->data_req_cap * 2;
        mod->data_reqs = grow_array(mod->arena, mod->data_reqs, mod->data_req_cap,
                                    new_cap, sizeof(DataRequirement));
        mod->data_req_cap = new_cap;
    }

    DataRequirement *req = &mod->data_reqs[mod->data_req_count];
    req->name = arena_strdup(mod->arena, name);
    req->query_ref = mod->data_req_count;
    req->query = arena_alloc(mod->arena, query_len + 1);
    memcpy(req->query, query, query_len);
    req->query[query_len] = '\0';
    req->query_len = query_len;
    req->line = line;
    req->column = column;
    req->param_count = param_count;
    req->param_names = NULL;
    req->param_slots = NULL;
    if (param_count > 0) {
        req->param_names = arena_alloc(mod->arena, sizeof(char *) * param_count);
        req->param_slots = arena_alloc(mod->arena, sizeof(uint16_t) * param_count);
        for (uint16_t i = 0; i < param_count; i++) {
            req->param_names[i] = arena_strdup(mod->arena, param_names[i]);
            req->param_slots[i] = param_slots[i];
        }
    }
    req->signature = data_req_signature(query, query_len, param_count, param_names);
    req->is_single = is_single;
    req->is_dynamic = is_dynamic;

    return (uint16_t)mod->data_req_count++;
}

uint16_t bytecode_add_dependency(BytecodeModule *mod, const char *path) {
    /* Check for existing */
    for (uint32_t i = 0; i < mod->dep_count; i++) {
        if (strcmp(mod->deps[i].path, path) == 0) {
            return (uint16_t)i;
        }
    }

    if (mod->dep_count >= mod->dep_cap) {
        uint32_t new_cap = mod->dep_cap == 0 ? 32 : mod->dep_cap * 2;
        mod->deps = grow_array(mod->arena, mod->deps, mod->dep_cap,
                               new_cap, sizeof(Dependency));
        mod->dep_cap = new_cap;
    }

    Dependency *dep = &mod->deps[mod->dep_count];
    dep->path = arena_strdup(mod->arena, path);
    dep->path_len = (uint32_t)strlen(path);

    return (uint16_t)mod->dep_count++;
}

uint16_t bytecode_add_builtin(BytecodeModule *mod, const char *name,
                              uint8_t min_args, uint8_t max_args) {
    /* Check for existing */
    int existing = bytecode_find_builtin(mod, name);
    if (existing >= 0) return (uint16_t)existing;

    if (mod->builtin_count >= mod->builtin_cap) {
        uint32_t new_cap = mod->builtin_cap == 0 ? 32 : mod->builtin_cap * 2;
        mod->builtins = grow_array(mod->arena, mod->builtins, mod->builtin_cap,
                                   new_cap, sizeof(BuiltinRef));
        mod->builtin_cap = new_cap;
    }

    BuiltinRef *ref = &mod->builtins[mod->builtin_count];
    ref->name = arena_strdup(mod->arena, name);
    ref->min_args = min_args;
    ref->max_args = max_args;

    return (uint16_t)mod->builtin_count++;
}

int bytecode_find_builtin(BytecodeModule *mod, const char *name) {
    for (uint32_t i = 0; i < mod->builtin_count; i++) {
        if (strcmp(mod->builtins[i].name, name) == 0) {
            return (int)i;
        }
    }
    return -1;
}

uint16_t bytecode_add_comp_ref(BytecodeModule *mod, const char *name, const char *path) {
    /* Check for existing */
    for (uint32_t i = 0; i < mod->comp_ref_count; i++) {
        if (strcmp(mod->comp_refs[i].name, name) == 0) {
            return (uint16_t)i;
        }
    }

    if (mod->comp_ref_count >= mod->comp_ref_cap) {
        uint32_t new_cap = mod->comp_ref_cap == 0 ? 16 : mod->comp_ref_cap * 2;
        mod->comp_refs = grow_array(mod->arena, mod->comp_refs, mod->comp_ref_cap,
                                    new_cap, sizeof(ComponentRef));
        mod->comp_ref_cap = new_cap;
    }

    ComponentRef *ref = &mod->comp_refs[mod->comp_ref_count];
    ref->name = arena_strdup(mod->arena, name);
    ref->path = arena_strdup(mod->arena, path);
    ref->name_len = (uint32_t)strlen(name);
    ref->path_len = (uint32_t)strlen(path);

    return (uint16_t)mod->comp_ref_count++;
}

uint16_t bytecode_add_function(BytecodeModule *mod) {
    if (mod->func_count >= mod->func_cap) {
        uint32_t new_cap = mod->func_cap == 0 ? 16 : mod->func_cap * 2;
        mod->functions = grow_array(mod->arena, mod->functions, mod->func_cap,
                                    new_cap, sizeof(Chunk));
        mod->func_debug = grow_array(mod->arena, mod->func_debug, mod->func_debug_cap,
                                     new_cap, sizeof(FunctionDebugRef));
        for (uint32_t i = mod->func_cap; i < new_cap; i++) {
            mod->func_debug[i].name = NULL;
            mod->func_debug[i].source_path = NULL;
            mod->func_debug[i].name_len = 0;
            mod->func_debug[i].source_path_len = 0;
        }
        mod->func_cap = new_cap;
        mod->func_debug_cap = new_cap;
    }

    chunk_init(&mod->functions[mod->func_count], mod->arena);
    return (uint16_t)mod->func_count++;
}

Chunk *bytecode_get_function(BytecodeModule *mod, uint16_t idx) {
    if (idx >= mod->func_count) return NULL;
    return &mod->functions[idx];
}

void bytecode_set_function_debug(BytecodeModule *mod, uint16_t idx,
                                 const char *name, const char *source_path) {
    if (!mod || idx >= mod->func_count || !mod->func_debug) return;

    FunctionDebugRef *ref = &mod->func_debug[idx];
    if (name && name[0] != '\0') {
        ref->name = arena_strdup(mod->arena, name);
        ref->name_len = (uint32_t)strlen(name);
    } else {
        ref->name = NULL;
        ref->name_len = 0;
    }

    if (source_path && source_path[0] != '\0') {
        ref->source_path = arena_strdup(mod->arena, source_path);
        ref->source_path_len = (uint32_t)strlen(source_path);
    } else {
        ref->source_path = NULL;
        ref->source_path_len = 0;
    }
}

const char *opcode_name(OpCode op) {
    switch (op) {
        case BC_NOP: return "NOP";
        case BC_CONST: return "CONST";
        case BC_POP: return "POP";
        case BC_DUP: return "DUP";
        case BC_LOAD: return "LOAD";
        case BC_STORE: return "STORE";
        case BC_LOAD_GLOBAL: return "LOAD_GLOBAL";
        case BC_LOAD_FIELD: return "LOAD_FIELD";
        case BC_LOAD_INDEX: return "LOAD_INDEX";
        case BC_STORE_FIELD: return "STORE_FIELD";
        case BC_STORE_INDEX: return "STORE_INDEX";
        case BC_NULL: return "NULL";
        case BC_TRUE: return "TRUE";
        case BC_FALSE: return "FALSE";
        case BC_INT: return "INT";
        case BC_ADD: return "ADD";
        case BC_SUB: return "SUB";
        case BC_MUL: return "MUL";
        case BC_DIV: return "DIV";
        case BC_MOD: return "MOD";
        case BC_NEG: return "NEG";
        case BC_EQ: return "EQ";
        case BC_NEQ: return "NEQ";
        case BC_LT: return "LT";
        case BC_LTE: return "LTE";
        case BC_GT: return "GT";
        case BC_GTE: return "GTE";
        case BC_AND: return "AND";
        case BC_OR: return "OR";
        case BC_NOT: return "NOT";
        case BC_JUMP: return "JUMP";
        case BC_JUMP_IF_FALSE: return "JUMP_IF_FALSE";
        case BC_JUMP_IF_TRUE: return "JUMP_IF_TRUE";
        case BC_ITER_START: return "ITER_START";
        case BC_ITER_NEXT: return "ITER_NEXT";
        case BC_ITER_END: return "ITER_END";
        case BC_EMIT_LITERAL: return "EMIT_LITERAL";
        case BC_EMIT_TEXT: return "EMIT_TEXT";
        case BC_EMIT_RAW: return "EMIT_RAW";
        case BC_EMIT_ATTR_START: return "EMIT_ATTR_START";
        case BC_EMIT_ATTR_END: return "EMIT_ATTR_END";
        case BC_EMIT_TAG_OPEN: return "EMIT_TAG_OPEN";
        case BC_EMIT_TAG_END: return "EMIT_TAG_END";
        case BC_EMIT_TAG_CLOSE: return "EMIT_TAG_CLOSE";
        case BC_EMIT_TAG_SELF: return "EMIT_TAG_SELF";
        case BC_FETCH_DATA: return "FETCH_DATA";
        case BC_FETCH_WAIT: return "FETCH_WAIT";
        case BC_CALL: return "CALL";
        case BC_CALL_BUILTIN: return "CALL_BUILTIN";
        case BC_CALL_PIPE: return "CALL_PIPE";
        case BC_RETURN: return "RETURN";
        case BC_COMPONENT_START: return "COMPONENT_START";
        case BC_COMPONENT_END: return "COMPONENT_END";
        case BC_COMPONENT_LOAD: return "COMPONENT_LOAD";
        case BC_SLOT_START: return "SLOT_START";
        case BC_SLOT_END: return "SLOT_END";
        case BC_SLOT_DEFAULT: return "SLOT_DEFAULT";
        case BC_DEP_START: return "DEP_START";
        case BC_DEP_END: return "DEP_END";
        case BC_ARRAY_NEW: return "ARRAY_NEW";
        case BC_OBJECT_NEW: return "OBJECT_NEW";
        case BC_OBJECT_SET: return "OBJECT_SET";
        case BC_CONCAT: return "CONCAT";
        case BC_HALT: return "HALT";
        case BC_COMPONENT_LINKED: return "COMPONENT_LINKED";
    }
    return "UNKNOWN";
}

/* Disassemble a single instruction, return bytes consumed */
#ifndef MOT_WASM_FREESTANDING
static int disassemble_instruction(Chunk *chunk, int offset, BytecodeModule *mod) {
    printf("%04d ", offset);

    /* Print line number */
    if (offset > 0 && chunk->lines[offset] == chunk->lines[offset - 1]) {
        printf("   | ");
    } else {
        printf("%4d ", chunk->lines[offset]);
    }

    uint8_t op = chunk->code[offset];
    printf("%-16s", opcode_name((OpCode)op));

    switch (op) {
        case BC_CONST:
        case BC_EMIT_LITERAL: {
            uint16_t idx = (chunk->code[offset + 1] << 8) | chunk->code[offset + 2];
            printf(" %d", idx);
            if (idx < mod->const_count) {
                Constant *c = &mod->constants[idx];
                printf(" (");
                switch (c->type) {
                    case CONST_NULL: printf("null"); break;
                    case CONST_BOOL: printf("%s", c->v.boolean ? "true" : "false"); break;
                    case CONST_INT: printf("%lld", (long long)c->v.integer); break;
                    case CONST_NUMBER: printf("%g", c->v.number); break;
                    case CONST_STRING: printf("\"%s\"", c->v.string.data); break;
                }
                printf(")");
            }
            printf("\n");
            return 3;
        }

        case BC_INT: {
            int16_t val = (int16_t)((chunk->code[offset + 1] << 8) | chunk->code[offset + 2]);
            printf(" %d\n", val);
            return 3;
        }

        case BC_LOAD:
        case BC_STORE:
        case BC_LOAD_GLOBAL:
        case BC_LOAD_FIELD:
        case BC_EMIT_ATTR_START:
        case BC_EMIT_TAG_OPEN:
        case BC_EMIT_TAG_CLOSE:
        case BC_EMIT_TAG_SELF:
        case BC_FETCH_DATA:
        case BC_COMPONENT_START:
        case BC_SLOT_START:
        case BC_DEP_START:
        case BC_ARRAY_NEW:
        case BC_OBJECT_NEW:
        case BC_OBJECT_SET: {
            uint16_t idx = (chunk->code[offset + 1] << 8) | chunk->code[offset + 2];
            printf(" %d\n", idx);
            return 3;
        }

        case BC_JUMP:
        case BC_JUMP_IF_FALSE:
        case BC_JUMP_IF_TRUE:
        case BC_ITER_NEXT: {
            int16_t jmp = (int16_t)((chunk->code[offset + 1] << 8) | chunk->code[offset + 2]);
            printf(" %d -> %d\n", jmp, offset + 3 + jmp);
            return 3;
        }

        case BC_CALL:
        case BC_CALL_BUILTIN:
        case BC_COMPONENT_LOAD:
        case BC_COMPONENT_LINKED: {
            uint16_t fn_idx = (chunk->code[offset + 1] << 8) | chunk->code[offset + 2];
            uint8_t argc = chunk->code[offset + 3];
            printf(" fn=%d argc=%d\n", fn_idx, argc);
            return 4;
        }

        case BC_CALL_PIPE: {
            uint16_t fn_idx = (chunk->code[offset + 1] << 8) | chunk->code[offset + 2];
            printf(" fn=%d\n", fn_idx);
            return 3;
        }

        case BC_CONCAT: {
            uint8_t count = chunk->code[offset + 1];
            printf(" %d\n", count);
            return 2;
        }

        default:
            printf("\n");
            return 1;
    }
}

void chunk_disassemble(Chunk *chunk, const char *name, BytecodeModule *mod) {
    printf("== %s ==\n", name);

    for (uint32_t offset = 0; offset < chunk->code_len;) {
        offset += disassemble_instruction(chunk, offset, mod);
    }
}

void bytecode_disassemble(BytecodeModule *mod) {
    printf("=== Bytecode Module ===\n");
    printf("Version: %d.%d\n", mod->version_major, mod->version_minor);
    printf("Constants: %d\n", mod->const_count);
    printf("Strings: %d\n", mod->string_count);
    printf("Data Requirements: %d\n", mod->data_req_count);
    printf("Dependencies: %d\n", mod->dep_count);
    printf("Functions: %d\n", mod->func_count);
    printf("\n");

    chunk_disassemble(&mod->main, "main", mod);

    for (uint32_t i = 0; i < mod->func_count; i++) {
        char name[32];
        snprintf(name, sizeof(name), "function_%d", i);
        chunk_disassemble(&mod->functions[i], name, mod);
    }
}
#else
void chunk_disassemble(Chunk *chunk, const char *name, BytecodeModule *mod) {
    (void)chunk;
    (void)name;
    (void)mod;
}

void bytecode_disassemble(BytecodeModule *mod) {
    (void)mod;
}
#endif

/* Serialization - write bytecode to buffer */
static void write_u8(uint8_t **buf, uint8_t val) {
    *(*buf)++ = val;
}

static void write_u16(uint8_t **buf, uint16_t val) {
    *(*buf)++ = val & 0xFF;          /* little-endian: low byte first */
    *(*buf)++ = (val >> 8) & 0xFF;
}

static void write_u32(uint8_t **buf, uint32_t val) {
    *(*buf)++ = val & 0xFF;          /* little-endian: low byte first */
    *(*buf)++ = (val >> 8) & 0xFF;
    *(*buf)++ = (val >> 16) & 0xFF;
    *(*buf)++ = (val >> 24) & 0xFF;
}

static void write_bytes(uint8_t **buf, const void *data, uint32_t len) {
    memcpy(*buf, data, len);
    *buf += len;
}

static bool read_u8(const uint8_t **ptr, const uint8_t *end, uint8_t *out) {
    if ((size_t)(end - *ptr) < 1) return false;
    *out = *(*ptr)++;
    return true;
}

static bool read_u16(const uint8_t **ptr, const uint8_t *end, uint16_t *out) {
    if ((size_t)(end - *ptr) < 2) return false;
    *out = (uint16_t)((*ptr)[0] | ((*ptr)[1] << 8));
    *ptr += 2;
    return true;
}

static bool read_u32(const uint8_t **ptr, const uint8_t *end, uint32_t *out) {
    if ((size_t)(end - *ptr) < 4) return false;
    *out = (uint32_t)((*ptr)[0] |
                      ((*ptr)[1] << 8) |
                      ((*ptr)[2] << 16) |
                      ((*ptr)[3] << 24));
    *ptr += 4;
    return true;
}

static bool read_i64(const uint8_t **ptr, const uint8_t *end, int64_t *out) {
    if ((size_t)(end - *ptr) < 8) return false;
    memcpy(out, *ptr, 8);
    *ptr += 8;
    return true;
}

static bool read_f64(const uint8_t **ptr, const uint8_t *end, double *out) {
    if ((size_t)(end - *ptr) < 8) return false;
    memcpy(out, *ptr, 8);
    *ptr += 8;
    return true;
}

static bool read_string_copy(const uint8_t **ptr, const uint8_t *end, Arena *arena,
                             char **out_str, uint32_t *out_len) {
    uint32_t len = 0;
    if (!read_u32(ptr, end, &len)) return false;
    if ((size_t)(end - *ptr) < len) return false;

    char *copy = arena_alloc(arena, len + 1);
    memcpy(copy, *ptr, len);
    copy[len] = '\0';
    *ptr += len;

    *out_str = copy;
    if (out_len) *out_len = len;
    return true;
}

static bool read_chunk(const uint8_t **ptr, const uint8_t *end, Arena *arena, Chunk *out_chunk) {
    uint32_t code_len = 0;
    if (!read_u32(ptr, end, &code_len)) return false;
    if ((size_t)(end - *ptr) < code_len) return false;

    uint32_t cap = code_len > 0 ? code_len : 1;
    out_chunk->code = arena_alloc(arena, cap);
    out_chunk->code_len = code_len;
    out_chunk->code_cap = cap;
    if (code_len > 0) {
        memcpy(out_chunk->code, *ptr, code_len);
    }
    *ptr += code_len;

    out_chunk->lines = arena_alloc(arena, cap * sizeof(uint32_t));
    out_chunk->lines_len = code_len;
    out_chunk->lines_cap = cap;
    for (uint32_t i = 0; i < code_len; i++) {
        out_chunk->lines[i] = 1;
    }

    return true;
}

static uint32_t count_chunk_line_spans(const Chunk *chunk) {
    if (!chunk || chunk->code_len == 0 || !chunk->lines) return 0;

    uint32_t count = 0;
    uint32_t i = 0;
    while (i < chunk->code_len) {
        uint32_t line = chunk->lines[i];
        count++;
        i++;
        while (i < chunk->code_len && chunk->lines[i] == line) {
            i++;
        }
    }
    return count;
}

static uint32_t count_function_debug_refs(const BytecodeModule *mod) {
    if (!mod || !mod->func_debug) return 0;
    uint32_t count = 0;
    for (uint32_t i = 0; i < mod->func_count; i++) {
        const FunctionDebugRef *ref = &mod->func_debug[i];
        if ((ref->name && ref->name_len > 0) ||
            (ref->source_path && ref->source_path_len > 0)) {
            count++;
        }
    }
    return count;
}

static void write_chunk_line_spans(uint8_t **ptr, const Chunk *chunk, uint8_t chunk_kind, uint16_t chunk_index) {
    if (!chunk || chunk->code_len == 0 || !chunk->lines) return;

    uint32_t i = 0;
    while (i < chunk->code_len) {
        uint32_t line = chunk->lines[i];
        uint32_t start_pc = i;
        i++;
        while (i < chunk->code_len && chunk->lines[i] == line) {
            i++;
        }
        uint32_t end_pc = i;

        write_u8(ptr, chunk_kind);      /* 0: main, 1: function */
        write_u16(ptr, chunk_index);    /* function index or 0 for main */
        write_u32(ptr, start_pc);       /* start bytecode offset in chunk */
        write_u32(ptr, end_pc);         /* end bytecode offset (exclusive) */
        write_u32(ptr, line);           /* source line */
        write_u32(ptr, 0);              /* source column (reserved) */
        write_u16(ptr, 0);              /* AST node type (reserved) */
    }
}

static bool read_debug_trailer(BytecodeModule *mod, const uint8_t **ptr, const uint8_t *end) {
    uint32_t magic = 0;
    if (!read_u32(ptr, end, &magic)) return false;
    if (magic != BYTECODE_DEBUG_MAGIC) return false;

    uint32_t span_count = 0;
    if (!read_u32(ptr, end, &span_count)) return false;
    mod->debug_span_count = span_count;
    if (span_count > 0) {
        mod->debug_spans = arena_alloc(mod->arena, sizeof(BytecodeDebugSpan) * span_count);
        if (!mod->debug_spans) return false;
    } else {
        mod->debug_spans = NULL;
    }
    for (uint32_t i = 0; i < span_count; i++) {
        BytecodeDebugSpan span = {0};
        if (!read_u8(ptr, end, &span.chunk_kind) ||
            !read_u16(ptr, end, &span.chunk_index) ||
            !read_u32(ptr, end, &span.start_pc) ||
            !read_u32(ptr, end, &span.end_pc) ||
            !read_u32(ptr, end, &span.line) ||
            !read_u32(ptr, end, &span.column) ||
            !read_u16(ptr, end, &span.node_type)) {
            return false;
        }
        if (mod->debug_spans) {
            mod->debug_spans[i] = span;
        }
    }

    uint32_t query_count = 0;
    if (!read_u32(ptr, end, &query_count)) return false;
    for (uint32_t i = 0; i < query_count; i++) {
        uint32_t query_ref = 0, signature = 0, line = 0, column = 0;
        uint16_t name_len = 0;
        if (!read_u32(ptr, end, &query_ref) ||
            !read_u32(ptr, end, &signature) ||
            !read_u32(ptr, end, &line) ||
            !read_u32(ptr, end, &column) ||
            !read_u16(ptr, end, &name_len)) {
            return false;
        }
        (void)query_ref;
        (void)signature;
        (void)line;
        (void)column;
        if ((size_t)(end - *ptr) < name_len) return false;
        *ptr += name_len;
    }

    if (*ptr == end) {
        return true;
    }

    uint32_t fn_ref_count = 0;
    if (!read_u32(ptr, end, &fn_ref_count)) return false;
    for (uint32_t i = 0; i < fn_ref_count; i++) {
        uint16_t func_idx = 0;
        uint16_t name_len = 0;
        uint16_t source_len = 0;
        if (!read_u16(ptr, end, &func_idx) ||
            !read_u16(ptr, end, &name_len)) {
            return false;
        }
        if ((size_t)(end - *ptr) < name_len) return false;
        const char *name_ptr = (const char *)(*ptr);
        *ptr += name_len;
        if (!read_u16(ptr, end, &source_len)) return false;
        if ((size_t)(end - *ptr) < source_len) return false;
        const char *src_ptr = (const char *)(*ptr);
        *ptr += source_len;
        if (func_idx < mod->func_count) {
            char *name_copy = arena_alloc(mod->arena, (size_t)name_len + 1u);
            char *src_copy = arena_alloc(mod->arena, (size_t)source_len + 1u);
            if (!name_copy || !src_copy) return false;
            memcpy(name_copy, name_ptr, name_len);
            name_copy[name_len] = '\0';
            memcpy(src_copy, src_ptr, source_len);
            src_copy[source_len] = '\0';
            bytecode_set_function_debug(mod, func_idx, name_copy, src_copy);
        }
    }

    return true;
}

uint8_t *bytecode_serialize_ex(BytecodeModule *mod, uint32_t *out_len, bool include_debug) {
    /* Calculate total size */
    uint32_t size = 0;

    /* Header: magic(4) + version(4) + flags(2) */
    size += 10;

    /* Section sizes */
    size += 4 * 7;  /* const, string, data_req, dep, builtin, comp_ref, func counts */

    /* Constants */
    for (uint32_t i = 0; i < mod->const_count; i++) {
        size += 1;  /* type */
        switch (mod->constants[i].type) {
            case CONST_NULL:
            case CONST_BOOL:
                size += 1;
                break;
            case CONST_INT:
                size += 8;
                break;
            case CONST_NUMBER:
                size += 8;
                break;
            case CONST_STRING:
                size += 4 + mod->constants[i].v.string.length;
                break;
        }
    }

    /* Strings */
    for (uint32_t i = 0; i < mod->string_count; i++) {
        size += 4 + (uint32_t)strlen(mod->strings[i]);
    }

    /* Data requirements */
    for (uint32_t i = 0; i < mod->data_req_count; i++) {
        size += 4 + (uint32_t)strlen(mod->data_reqs[i].name);
        size += 2;  /* flags */
        size += 4;  /* query_ref */
        size += 4;  /* signature */
        size += 2;  /* param_count */
        for (uint16_t p = 0; p < mod->data_reqs[i].param_count; p++) {
            size += 4 + (uint32_t)strlen(mod->data_reqs[i].param_names[p]);
            size += 2;  /* param slot */
        }
    }

    /* Dependencies */
    for (uint32_t i = 0; i < mod->dep_count; i++) {
        size += 4 + mod->deps[i].path_len;
    }

    /* Builtins */
    for (uint32_t i = 0; i < mod->builtin_count; i++) {
        size += 4 + (uint32_t)strlen(mod->builtins[i].name);
        size += 2;  /* min/max args */
    }

    /* Component refs */
    for (uint32_t i = 0; i < mod->comp_ref_count; i++) {
        size += 4 + mod->comp_refs[i].name_len;
        size += 4 + mod->comp_refs[i].path_len;
    }

    /* Main chunk */
    size += 4 + mod->main.code_len;

    /* Function chunks */
    for (uint32_t i = 0; i < mod->func_count; i++) {
        size += 4 + mod->functions[i].code_len;
    }

    uint32_t span_count = 0;
    uint32_t fn_debug_count = 0;
    if (include_debug) {
        /* Optional debug trailer:
         * magic(u32), span_count(u32), spans, query_count(u32), query entries,
         * function_debug_count(u32), function debug entries. */
        span_count = count_chunk_line_spans(&mod->main);
        for (uint32_t i = 0; i < mod->func_count; i++) {
            span_count += count_chunk_line_spans(&mod->functions[i]);
        }
        fn_debug_count = count_function_debug_refs(mod);
        size += 4;  /* debug magic */
        size += 4;  /* span count */
        size += span_count * (1 + 2 + 4 + 4 + 4 + 4 + 2);
        size += 4;  /* query count */
        for (uint32_t i = 0; i < mod->data_req_count; i++) {
            uint32_t name_len = (uint32_t)strlen(mod->data_reqs[i].name);
            if (name_len > 0xFFFFu) name_len = 0xFFFFu;
            size += 4 + 4 + 4 + 4 + 2 + name_len;
        }
        size += 4;  /* function debug count */
        if (mod->func_debug) {
            for (uint32_t i = 0; i < mod->func_count; i++) {
                FunctionDebugRef *ref = &mod->func_debug[i];
                if ((ref->name && ref->name_len > 0) ||
                    (ref->source_path && ref->source_path_len > 0)) {
                    uint16_t name_len = ref->name_len > 0xFFFFu ? 0xFFFFu : (uint16_t)ref->name_len;
                    uint16_t source_len = ref->source_path_len > 0xFFFFu ? 0xFFFFu : (uint16_t)ref->source_path_len;
                    size += 2 + 2 + name_len + 2 + source_len;
                }
            }
        }
    }

    /* Allocate buffer */
    uint8_t *buffer = arena_alloc(mod->arena, size);
    uint8_t *ptr = buffer;

    /* Write header */
    write_u32(&ptr, mod->magic);
    write_u16(&ptr, mod->version_major);
    write_u16(&ptr, mod->version_minor);
    write_u16(&ptr, mod->flags);

    /* Write section counts */
    write_u32(&ptr, mod->const_count);
    write_u32(&ptr, mod->string_count);
    write_u32(&ptr, mod->data_req_count);
    write_u32(&ptr, mod->dep_count);
    write_u32(&ptr, mod->builtin_count);
    write_u32(&ptr, mod->comp_ref_count);
    write_u32(&ptr, mod->func_count);

    /* Write constants */
    for (uint32_t i = 0; i < mod->const_count; i++) {
        Constant *c = &mod->constants[i];
        write_u8(&ptr, c->type);
        switch (c->type) {
            case CONST_NULL:
                write_u8(&ptr, 0);
                break;
            case CONST_BOOL:
                write_u8(&ptr, c->v.boolean ? 1 : 0);
                break;
            case CONST_INT:
                write_bytes(&ptr, &c->v.integer, 8);
                break;
            case CONST_NUMBER:
                write_bytes(&ptr, &c->v.number, 8);
                break;
            case CONST_STRING:
                write_u32(&ptr, c->v.string.length);
                write_bytes(&ptr, c->v.string.data, c->v.string.length);
                break;
        }
    }

    /* Write strings */
    for (uint32_t i = 0; i < mod->string_count; i++) {
        uint32_t len = (uint32_t)strlen(mod->strings[i]);
        write_u32(&ptr, len);
        write_bytes(&ptr, mod->strings[i], len);
    }

    /* Write data requirements */
    for (uint32_t i = 0; i < mod->data_req_count; i++) {
        DataRequirement *req = &mod->data_reqs[i];
        uint32_t name_len = (uint32_t)strlen(req->name);
        write_u32(&ptr, name_len);
        write_bytes(&ptr, req->name, name_len);
        write_u8(&ptr, req->is_single ? 1 : 0);
        write_u8(&ptr, req->is_dynamic ? 1 : 0);
        write_u32(&ptr, req->query_ref);
        write_u32(&ptr, req->signature);
        write_u16(&ptr, req->param_count);
        for (uint16_t p = 0; p < req->param_count; p++) {
            uint32_t param_name_len = (uint32_t)strlen(req->param_names[p]);
            write_u32(&ptr, param_name_len);
            write_bytes(&ptr, req->param_names[p], param_name_len);
            write_u16(&ptr, req->param_slots[p]);
        }
    }

    /* Write dependencies */
    for (uint32_t i = 0; i < mod->dep_count; i++) {
        write_u32(&ptr, mod->deps[i].path_len);
        write_bytes(&ptr, mod->deps[i].path, mod->deps[i].path_len);
    }

    /* Write builtins */
    for (uint32_t i = 0; i < mod->builtin_count; i++) {
        BuiltinRef *ref = &mod->builtins[i];
        uint32_t name_len = (uint32_t)strlen(ref->name);
        write_u32(&ptr, name_len);
        write_bytes(&ptr, ref->name, name_len);
        write_u8(&ptr, ref->min_args);
        write_u8(&ptr, ref->max_args);
    }

    /* Write component refs */
    for (uint32_t i = 0; i < mod->comp_ref_count; i++) {
        ComponentRef *ref = &mod->comp_refs[i];
        write_u32(&ptr, ref->name_len);
        write_bytes(&ptr, ref->name, ref->name_len);
        write_u32(&ptr, ref->path_len);
        write_bytes(&ptr, ref->path, ref->path_len);
    }

    /* Write main chunk */
    write_u32(&ptr, mod->main.code_len);
    write_bytes(&ptr, mod->main.code, mod->main.code_len);

    /* Write function chunks */
    for (uint32_t i = 0; i < mod->func_count; i++) {
        write_u32(&ptr, mod->functions[i].code_len);
        write_bytes(&ptr, mod->functions[i].code, mod->functions[i].code_len);
    }

    if (include_debug) {
        /* Write optional debug trailer. */
        write_u32(&ptr, BYTECODE_DEBUG_MAGIC);
        write_u32(&ptr, span_count);
        write_chunk_line_spans(&ptr, &mod->main, 0, 0);
        for (uint32_t i = 0; i < mod->func_count; i++) {
            write_chunk_line_spans(&ptr, &mod->functions[i], 1, (uint16_t)i);
        }

        write_u32(&ptr, mod->data_req_count);
        for (uint32_t i = 0; i < mod->data_req_count; i++) {
            DataRequirement *req = &mod->data_reqs[i];
            uint32_t name_len_u32 = (uint32_t)strlen(req->name);
            uint16_t name_len = (name_len_u32 > 0xFFFFu) ? 0xFFFFu : (uint16_t)name_len_u32;
            write_u32(&ptr, req->query_ref);
            write_u32(&ptr, req->signature);
            write_u32(&ptr, req->line);
            write_u32(&ptr, req->column);
            write_u16(&ptr, name_len);
            write_bytes(&ptr, req->name, name_len);
        }

        write_u32(&ptr, fn_debug_count);
        if (mod->func_debug) {
            for (uint32_t i = 0; i < mod->func_count; i++) {
                FunctionDebugRef *ref = &mod->func_debug[i];
                if (!((ref->name && ref->name_len > 0) ||
                      (ref->source_path && ref->source_path_len > 0))) {
                    continue;
                }
                uint16_t name_len = ref->name_len > 0xFFFFu ? 0xFFFFu : (uint16_t)ref->name_len;
                uint16_t source_len = ref->source_path_len > 0xFFFFu ? 0xFFFFu : (uint16_t)ref->source_path_len;
                write_u16(&ptr, (uint16_t)i);
                write_u16(&ptr, name_len);
                if (name_len > 0) {
                    write_bytes(&ptr, ref->name, name_len);
                }
                write_u16(&ptr, source_len);
                if (source_len > 0) {
                    write_bytes(&ptr, ref->source_path, source_len);
                }
            }
        }
    }

    *out_len = size;
    return buffer;
}

uint8_t *bytecode_serialize(BytecodeModule *mod, uint32_t *out_len) {
    return bytecode_serialize_ex(mod, out_len, true);
}

BytecodeModule *bytecode_deserialize(const uint8_t *data, uint32_t len, Arena *arena) {
    if (!data || !arena) return NULL;

    const uint8_t *ptr = data;
    const uint8_t *end = data + len;

    uint32_t magic = 0;
    uint16_t version_major = 0;
    uint16_t version_minor = 0;
    uint16_t flags = 0;

    if (!read_u32(&ptr, end, &magic) ||
        !read_u16(&ptr, end, &version_major) ||
        !read_u16(&ptr, end, &version_minor) ||
        !read_u16(&ptr, end, &flags)) {
        return NULL;
    }

    if (magic != BYTECODE_MAGIC) {
        return NULL;
    }
    if (version_major != BYTECODE_VERSION_MAJOR ||
        version_minor != BYTECODE_VERSION_MINOR) {
        return NULL;
    }

    BytecodeModule *mod = bytecode_module_new(arena);
    mod->magic = magic;
    mod->version_major = version_major;
    mod->version_minor = version_minor;
    mod->flags = flags;

    uint32_t const_count = 0, string_count = 0, data_req_count = 0;
    uint32_t dep_count = 0, builtin_count = 0, comp_ref_count = 0, func_count = 0;
    if (!read_u32(&ptr, end, &const_count) ||
        !read_u32(&ptr, end, &string_count) ||
        !read_u32(&ptr, end, &data_req_count) ||
        !read_u32(&ptr, end, &dep_count) ||
        !read_u32(&ptr, end, &builtin_count) ||
        !read_u32(&ptr, end, &comp_ref_count) ||
        !read_u32(&ptr, end, &func_count)) {
        return NULL;
    }

    mod->const_count = const_count;
    mod->const_cap = const_count;
    if (const_count > 0) {
        mod->constants = arena_alloc(arena, sizeof(Constant) * const_count);
    } else {
        mod->constants = NULL;
    }

    for (uint32_t i = 0; i < const_count; i++) {
        uint8_t type_u8 = 0;
        if (!read_u8(&ptr, end, &type_u8) || type_u8 > CONST_STRING) {
            return NULL;
        }
        Constant *c = &mod->constants[i];
        c->type = (ConstType)type_u8;
        switch (c->type) {
            case CONST_NULL: {
                uint8_t dummy = 0;
                if (!read_u8(&ptr, end, &dummy)) return NULL;
                break;
            }
            case CONST_BOOL: {
                uint8_t b = 0;
                if (!read_u8(&ptr, end, &b)) return NULL;
                c->v.boolean = b != 0;
                break;
            }
            case CONST_INT:
                if (!read_i64(&ptr, end, &c->v.integer)) return NULL;
                break;
            case CONST_NUMBER:
                if (!read_f64(&ptr, end, &c->v.number)) return NULL;
                break;
            case CONST_STRING:
                if (!read_string_copy(&ptr, end, arena, &c->v.string.data, &c->v.string.length)) {
                    return NULL;
                }
                break;
        }
    }

    mod->string_count = string_count;
    mod->string_cap = string_count;
    if (string_count > 0) {
        mod->strings = arena_alloc(arena, sizeof(char *) * string_count);
    } else {
        mod->strings = NULL;
    }
    for (uint32_t i = 0; i < string_count; i++) {
        if (!read_string_copy(&ptr, end, arena, &mod->strings[i], NULL)) {
            return NULL;
        }
    }

    mod->data_req_count = data_req_count;
    mod->data_req_cap = data_req_count;
    if (data_req_count > 0) {
        mod->data_reqs = arena_alloc(arena, sizeof(DataRequirement) * data_req_count);
    } else {
        mod->data_reqs = NULL;
    }
    for (uint32_t i = 0; i < data_req_count; i++) {
        DataRequirement *req = &mod->data_reqs[i];
        if (!read_string_copy(&ptr, end, arena, &req->name, NULL)) return NULL;
        req->query_len = 0;
        req->query = arena_alloc(arena, 1);
        req->query[0] = '\0';
        req->line = 0;
        req->column = 0;
        uint8_t is_single = 0;
        uint8_t is_dynamic = 0;
        if (!read_u8(&ptr, end, &is_single) || !read_u8(&ptr, end, &is_dynamic)) return NULL;
        req->is_single = is_single != 0;
        req->is_dynamic = is_dynamic != 0;
        if (!read_u32(&ptr, end, &req->query_ref) ||
            !read_u32(&ptr, end, &req->signature) ||
            !read_u16(&ptr, end, &req->param_count)) {
            return NULL;
        }
        req->param_names = NULL;
        req->param_slots = NULL;
        if (req->param_count > 0) {
            req->param_names = arena_alloc(arena, sizeof(char *) * req->param_count);
            req->param_slots = arena_alloc(arena, sizeof(uint16_t) * req->param_count);
            for (uint16_t p = 0; p < req->param_count; p++) {
                if (!read_string_copy(&ptr, end, arena, &req->param_names[p], NULL) ||
                    !read_u16(&ptr, end, &req->param_slots[p])) {
                    return NULL;
                }
            }
        }
    }

    mod->dep_count = dep_count;
    mod->dep_cap = dep_count;
    if (dep_count > 0) {
        mod->deps = arena_alloc(arena, sizeof(Dependency) * dep_count);
    } else {
        mod->deps = NULL;
    }
    for (uint32_t i = 0; i < dep_count; i++) {
        if (!read_string_copy(&ptr, end, arena, &mod->deps[i].path, &mod->deps[i].path_len)) {
            return NULL;
        }
    }

    mod->builtin_count = builtin_count;
    mod->builtin_cap = builtin_count;
    if (builtin_count > 0) {
        mod->builtins = arena_alloc(arena, sizeof(BuiltinRef) * builtin_count);
    } else {
        mod->builtins = NULL;
    }
    for (uint32_t i = 0; i < builtin_count; i++) {
        BuiltinRef *ref = &mod->builtins[i];
        if (!read_string_copy(&ptr, end, arena, &ref->name, NULL)) return NULL;
        if (!read_u8(&ptr, end, &ref->min_args) || !read_u8(&ptr, end, &ref->max_args)) {
            return NULL;
        }
    }

    mod->comp_ref_count = comp_ref_count;
    mod->comp_ref_cap = comp_ref_count;
    if (comp_ref_count > 0) {
        mod->comp_refs = arena_alloc(arena, sizeof(ComponentRef) * comp_ref_count);
    } else {
        mod->comp_refs = NULL;
    }
    for (uint32_t i = 0; i < comp_ref_count; i++) {
        ComponentRef *ref = &mod->comp_refs[i];
        if (!read_string_copy(&ptr, end, arena, &ref->name, &ref->name_len)) return NULL;
        if (!read_string_copy(&ptr, end, arena, &ref->path, &ref->path_len)) return NULL;
    }

    if (!read_chunk(&ptr, end, arena, &mod->main)) {
        return NULL;
    }

    mod->func_count = func_count;
    mod->func_cap = func_count;
    if (func_count > 0) {
        mod->functions = arena_alloc(arena, sizeof(Chunk) * func_count);
        for (uint32_t i = 0; i < func_count; i++) {
            if (!read_chunk(&ptr, end, arena, &mod->functions[i])) {
                return NULL;
            }
        }
    } else {
        mod->functions = NULL;
    }

    if (ptr != end) {
        if (!read_debug_trailer(mod, &ptr, end)) {
            return NULL;
        }
        if (ptr != end) {
            return NULL;
        }
    }

    return mod;
}

bool bytecode_find_debug_span(const BytecodeModule *mod,
                              uint8_t chunk_kind,
                              uint16_t chunk_index,
                              uint32_t pc,
                              BytecodeDebugSpan *out_span) {
    if (!mod || !mod->debug_spans || mod->debug_span_count == 0) return false;
    for (uint32_t i = 0; i < mod->debug_span_count; i++) {
        const BytecodeDebugSpan *span = &mod->debug_spans[i];
        if (span->chunk_kind != chunk_kind) continue;
        if (span->chunk_index != chunk_index) continue;
        if (pc < span->start_pc || pc >= span->end_pc) continue;
        if (out_span) *out_span = *span;
        return true;
    }
    return false;
}
