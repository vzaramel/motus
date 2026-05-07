/*
 * Bytecode linker for BC_COMPONENT_LINKED.
 *
 * This pass merges linked component function chunks into the current module and
 * records per-ref target mappings as internal dependency markers:
 *   "@linked_ref:<ref_idx>:<func_idx>"
 */

#include "bytecode_linker.h"

#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <stdio.h>

#define LINKED_NONE 0xFFFFu
#define LINKED_MARKER_PREFIX "@linked_ref:"

typedef struct PathCacheEntry {
    const char *path;
    uint16_t func_idx;
    struct PathCacheEntry *next;
} PathCacheEntry;

typedef struct PathStackEntry {
    const char *path;
    struct PathStackEntry *next;
} PathStackEntry;

typedef struct {
    BytecodeModule *module;
    MotLinkedModuleResolverFn resolver;
    void *resolver_userdata;
    char *error_buf;
    size_t error_buf_len;

    uint16_t *linked_targets;
    uint32_t linked_cap;

    PathCacheEntry *cache;
    PathStackEntry *stack;
} LinkerCtx;

typedef struct {
    uint16_t *const_map;
    uint16_t *string_map;
    uint16_t *data_req_map;
    uint16_t *dep_map;
    uint16_t *builtin_map;
    uint16_t *comp_ref_map;
    uint16_t *func_map;
} MergeMaps;

static void set_error(LinkerCtx *ctx, const char *msg) {
    if (!ctx || !ctx->error_buf || ctx->error_buf_len == 0) return;
    if (!msg) {
        ctx->error_buf[0] = '\0';
        return;
    }
    (void)snprintf(ctx->error_buf, ctx->error_buf_len, "%s", msg);
}

static bool str_eq(const char *a, const char *b) {
    if (!a || !b) return false;
    return strcmp(a, b) == 0;
}

static uint16_t read_u16_le(const uint8_t *p) {
    return (uint16_t)((uint16_t)p[0] | ((uint16_t)p[1] << 8));
}

static void write_u16_le(uint8_t *p, uint16_t v) {
    p[0] = (uint8_t)(v & 0xffu);
    p[1] = (uint8_t)((v >> 8) & 0xffu);
}

static int opcode_operand_size(uint8_t op) {
    switch ((OpCode)op) {
        case BC_CONST:
        case BC_LOAD:
        case BC_STORE:
        case BC_LOAD_GLOBAL:
        case BC_LOAD_FIELD:
        case BC_STORE_FIELD:
        case BC_INT:
        case BC_JUMP:
        case BC_JUMP_IF_FALSE:
        case BC_JUMP_IF_TRUE:
        case BC_ITER_NEXT:
        case BC_EMIT_LITERAL:
        case BC_EMIT_ATTR_START:
        case BC_EMIT_TAG_OPEN:
        case BC_EMIT_TAG_CLOSE:
        case BC_FETCH_DATA:
        case BC_COMPONENT_START:
        case BC_SLOT_START:
        case BC_DEP_START:
        case BC_ARRAY_NEW:
        case BC_OBJECT_NEW:
        case BC_OBJECT_SET:
            return 2;
        case BC_CALL:
        case BC_CALL_BUILTIN:
        case BC_COMPONENT_LOAD:
        case BC_COMPONENT_LINKED:
            return 3;
        case BC_CALL_PIPE:
            return 2;
        case BC_CONCAT:
            return 1;
        default:
            return 0;
    }
}

static bool ensure_linked_targets(LinkerCtx *ctx, uint32_t count) {
    uint32_t old_cap;
    uint16_t *next;
    if (!ctx) return false;
    if (count <= ctx->linked_cap) return true;

    old_cap = ctx->linked_cap;
    next = (uint16_t *)realloc(ctx->linked_targets, sizeof(uint16_t) * (size_t)count);
    if (!next) {
        set_error(ctx, "out of memory expanding linked target table");
        return false;
    }
    ctx->linked_targets = next;
    ctx->linked_cap = count;
    for (uint32_t i = old_cap; i < count; i++) {
        ctx->linked_targets[i] = LINKED_NONE;
    }
    return true;
}

static bool parse_u32_dec(const char *s, const char **out_end, uint32_t *out) {
    uint64_t v = 0;
    const char *p = s;
    if (!p || *p < '0' || *p > '9') return false;
    while (*p >= '0' && *p <= '9') {
        v = v * 10u + (uint64_t)(*p - '0');
        if (v > 0xFFFFFFFFu) return false;
        p++;
    }
    if (out_end) *out_end = p;
    if (out) *out = (uint32_t)v;
    return true;
}

static bool parse_linked_marker(const char *s, uint32_t *out_ref, uint32_t *out_func) {
    const char *p = s;
    uint32_t ref_idx = 0;
    uint32_t func_idx = 0;
    if (!p) return false;
    if (strncmp(p, LINKED_MARKER_PREFIX, strlen(LINKED_MARKER_PREFIX)) != 0) return false;
    p += strlen(LINKED_MARKER_PREFIX);
    if (!parse_u32_dec(p, &p, &ref_idx)) return false;
    if (*p != ':') return false;
    p++;
    if (!parse_u32_dec(p, &p, &func_idx)) return false;
    if (*p != '\0') return false;
    if (out_ref) *out_ref = ref_idx;
    if (out_func) *out_func = func_idx;
    return true;
}

static bool set_linked_target(LinkerCtx *ctx, uint16_t ref_idx, uint16_t func_idx) {
    char marker[64];
    if (!ctx || !ctx->module) return false;
    if (!ensure_linked_targets(ctx, ctx->module->comp_ref_count)) return false;
    if (ref_idx >= ctx->linked_cap) return false;
    ctx->linked_targets[ref_idx] = func_idx;
    (void)snprintf(marker, sizeof(marker), "%s%u:%u",
                   LINKED_MARKER_PREFIX, (unsigned)ref_idx, (unsigned)func_idx);
    (void)bytecode_add_dependency(ctx->module, marker);
    return true;
}

static void load_existing_markers(LinkerCtx *ctx) {
    if (!ctx || !ctx->module) return;
    if (!ensure_linked_targets(ctx, ctx->module->comp_ref_count)) return;
    for (uint32_t i = 0; i < ctx->module->dep_count; i++) {
        uint32_t ref_idx = 0, func_idx = 0;
        if (!parse_linked_marker(ctx->module->deps[i].path, &ref_idx, &func_idx)) continue;
        if (ref_idx >= ctx->module->comp_ref_count) continue;
        if (func_idx > 0xFFFFu) continue;
        ctx->linked_targets[ref_idx] = (uint16_t)func_idx;
    }
}

static bool path_on_stack(LinkerCtx *ctx, const char *path) {
    for (PathStackEntry *it = ctx ? ctx->stack : NULL; it; it = it->next) {
        if (str_eq(it->path, path)) return true;
    }
    return false;
}

static bool push_path(LinkerCtx *ctx, const char *path) {
    PathStackEntry *entry;
    if (!ctx || !path) return false;
    entry = (PathStackEntry *)malloc(sizeof(PathStackEntry));
    if (!entry) {
        set_error(ctx, "out of memory tracking linker recursion stack");
        return false;
    }
    entry->path = path;
    entry->next = ctx->stack;
    ctx->stack = entry;
    return true;
}

static void pop_path(LinkerCtx *ctx) {
    PathStackEntry *entry;
    if (!ctx || !ctx->stack) return;
    entry = ctx->stack;
    ctx->stack = entry->next;
    free(entry);
}

static bool source_path_matches_component(const char *source_path, const char *component_path) {
    size_t src_len;
    size_t comp_len;
    if (!source_path || !component_path) return false;
    src_len = strlen(source_path);
    comp_len = strlen(component_path);
    if (src_len >= 4 && strcmp(source_path + src_len - 4, ".mot") == 0) {
        src_len -= 4;
    }
    if (src_len != comp_len) return false;
    return strncmp(source_path, component_path, src_len) == 0;
}

static uint16_t choose_component_entry_function(const BytecodeModule *src,
                                                const char *component_name,
                                                const char *component_path) {
    if (!src || src->func_count == 0) return LINKED_NONE;
    if (src->func_debug) {
        for (uint32_t i = 0; i < src->func_count; i++) {
            const FunctionDebugRef *fd = &src->func_debug[i];
            if (!fd->name || !fd->source_path) continue;
            if (!str_eq(fd->name, component_name)) continue;
            if (!source_path_matches_component(fd->source_path, component_path)) continue;
            return (uint16_t)i;
        }
        for (uint32_t i = 0; i < src->func_count; i++) {
            const FunctionDebugRef *fd = &src->func_debug[i];
            if (!fd->source_path) continue;
            if (source_path_matches_component(fd->source_path, component_path)) {
                return (uint16_t)i;
            }
        }
    }
    return 0;
}

static void free_merge_maps(MergeMaps *m) {
    if (!m) return;
    free(m->const_map);
    free(m->string_map);
    free(m->data_req_map);
    free(m->dep_map);
    free(m->builtin_map);
    free(m->comp_ref_map);
    free(m->func_map);
    memset(m, 0, sizeof(*m));
}

static bool alloc_map(uint16_t **out, uint32_t count, LinkerCtx *ctx) {
    if (!out) return false;
    if (count == 0) {
        *out = NULL;
        return true;
    }
    *out = (uint16_t *)malloc(sizeof(uint16_t) * (size_t)count);
    if (!*out) {
        set_error(ctx, "out of memory allocating linker remap table");
        return false;
    }
    return true;
}

static bool ensure_ref_resolved(LinkerCtx *ctx, uint16_t ref_idx);

static bool remap_chunk_code(LinkerCtx *ctx,
                             const BytecodeModule *src,
                             const MergeMaps *maps,
                             const Chunk *src_chunk,
                             Chunk *dst_chunk) {
    uint8_t *code;
    if (!ctx || !src || !maps || !src_chunk || !dst_chunk) return false;
    if (src_chunk->code_len == 0) {
        dst_chunk->code = NULL;
        dst_chunk->code_len = 0;
        dst_chunk->code_cap = 0;
    } else {
        code = (uint8_t *)arena_alloc(ctx->module->arena, src_chunk->code_len);
        if (!code) {
            set_error(ctx, "out of memory copying linked function chunk");
            return false;
        }
        memcpy(code, src_chunk->code, src_chunk->code_len);
        dst_chunk->code = code;
        dst_chunk->code_len = src_chunk->code_len;
        dst_chunk->code_cap = src_chunk->code_len;
    }

    if (src_chunk->lines_len == 0) {
        dst_chunk->lines = NULL;
        dst_chunk->lines_len = 0;
        dst_chunk->lines_cap = 0;
    } else {
        uint32_t *lines = (uint32_t *)arena_alloc(ctx->module->arena, sizeof(uint32_t) * src_chunk->lines_len);
        if (!lines) {
            set_error(ctx, "out of memory copying linked function line table");
            return false;
        }
        memcpy(lines, src_chunk->lines, sizeof(uint32_t) * src_chunk->lines_len);
        dst_chunk->lines = lines;
        dst_chunk->lines_len = src_chunk->lines_len;
        dst_chunk->lines_cap = src_chunk->lines_len;
    }

    for (uint32_t ip = 0; ip < dst_chunk->code_len;) {
        uint8_t op = dst_chunk->code[ip];
        int operand_size = opcode_operand_size(op);
        uint32_t insn_size = 1u + (uint32_t)((operand_size < 0) ? 0 : operand_size);
        if (ip + insn_size > dst_chunk->code_len) {
            set_error(ctx, "invalid opcode stream while remapping linked chunk");
            return false;
        }

        switch ((OpCode)op) {
            case BC_CONST: {
                uint16_t idx = read_u16_le(dst_chunk->code + ip + 1u);
                if (idx >= src->const_count) { set_error(ctx, "invalid BC_CONST index in linked chunk"); return false; }
                write_u16_le(dst_chunk->code + ip + 1u, maps->const_map[idx]);
                break;
            }
            case BC_EMIT_LITERAL: {
                uint16_t idx = read_u16_le(dst_chunk->code + ip + 1u);
                if (idx >= src->const_count) { set_error(ctx, "invalid BC_EMIT_LITERAL index in linked chunk"); return false; }
                write_u16_le(dst_chunk->code + ip + 1u, maps->const_map[idx]);
                break;
            }
            case BC_LOAD_GLOBAL:
            case BC_LOAD_FIELD:
            case BC_STORE_FIELD:
            case BC_EMIT_ATTR_START:
            case BC_EMIT_TAG_OPEN:
            case BC_EMIT_TAG_CLOSE:
            case BC_OBJECT_SET: {
                uint16_t idx = read_u16_le(dst_chunk->code + ip + 1u);
                if (idx >= src->string_count) { set_error(ctx, "invalid string index in linked chunk"); return false; }
                write_u16_le(dst_chunk->code + ip + 1u, maps->string_map[idx]);
                break;
            }
            case BC_FETCH_DATA: {
                uint16_t idx = read_u16_le(dst_chunk->code + ip + 1u);
                if (idx >= src->data_req_count) { set_error(ctx, "invalid BC_FETCH_DATA index in linked chunk"); return false; }
                write_u16_le(dst_chunk->code + ip + 1u, maps->data_req_map[idx]);
                break;
            }
            case BC_DEP_START: {
                uint16_t idx = read_u16_le(dst_chunk->code + ip + 1u);
                uint16_t mapped;
                if (idx >= src->dep_count) { set_error(ctx, "invalid BC_DEP_START index in linked chunk"); return false; }
                mapped = maps->dep_map[idx];
                if (mapped == LINKED_NONE) {
                    set_error(ctx, "linked marker dependency referenced by BC_DEP_START in linked chunk");
                    return false;
                }
                write_u16_le(dst_chunk->code + ip + 1u, mapped);
                break;
            }
            case BC_CALL_BUILTIN: {
                uint16_t idx = read_u16_le(dst_chunk->code + ip + 1u);
                if (idx >= src->builtin_count) { set_error(ctx, "invalid BC_CALL_BUILTIN index in linked chunk"); return false; }
                write_u16_le(dst_chunk->code + ip + 1u, maps->builtin_map[idx]);
                break;
            }
            case BC_CALL:
            case BC_CALL_PIPE:
            case BC_COMPONENT_START: {
                uint16_t idx = read_u16_le(dst_chunk->code + ip + 1u);
                if (idx >= src->func_count) { set_error(ctx, "invalid function index in linked chunk"); return false; }
                write_u16_le(dst_chunk->code + ip + 1u, maps->func_map[idx]);
                break;
            }
            case BC_COMPONENT_LOAD:
            case BC_COMPONENT_LINKED: {
                uint16_t idx = read_u16_le(dst_chunk->code + ip + 1u);
                uint16_t mapped;
                if (idx >= src->comp_ref_count) { set_error(ctx, "invalid component ref index in linked chunk"); return false; }
                mapped = maps->comp_ref_map[idx];
                write_u16_le(dst_chunk->code + ip + 1u, mapped);
                if ((OpCode)op == BC_COMPONENT_LINKED) {
                    if (!ensure_ref_resolved(ctx, mapped)) return false;
                }
                break;
            }
            default:
                break;
        }

        ip += insn_size;
    }

    return true;
}

static bool merge_component_module(LinkerCtx *ctx,
                                   const BytecodeModule *src,
                                   const char *component_name,
                                   const char *component_path,
                                   uint16_t *out_entry_func_idx) {
    MergeMaps maps;
    uint16_t src_entry_idx;
    if (!ctx || !src || !out_entry_func_idx) return false;
    memset(&maps, 0, sizeof(maps));
    *out_entry_func_idx = LINKED_NONE;

    if (src->func_count == 0) {
        set_error(ctx, "linked component module has no function chunks");
        return false;
    }

    if (!alloc_map(&maps.const_map, src->const_count, ctx) ||
        !alloc_map(&maps.string_map, src->string_count, ctx) ||
        !alloc_map(&maps.data_req_map, src->data_req_count, ctx) ||
        !alloc_map(&maps.dep_map, src->dep_count, ctx) ||
        !alloc_map(&maps.builtin_map, src->builtin_count, ctx) ||
        !alloc_map(&maps.comp_ref_map, src->comp_ref_count, ctx) ||
        !alloc_map(&maps.func_map, src->func_count, ctx)) {
        free_merge_maps(&maps);
        return false;
    }

    for (uint32_t i = 0; i < src->const_count; i++) {
        Constant c = src->constants[i];
        if (c.type == CONST_STRING) {
            uint16_t intern_idx = bytecode_add_string(
                ctx->module,
                c.v.string.data ? c.v.string.data : "",
                c.v.string.length
            );
            if (intern_idx >= ctx->module->string_count || !ctx->module->strings[intern_idx]) {
                set_error(ctx, "failed to intern linked constant string");
                free_merge_maps(&maps);
                return false;
            }
            c.v.string.data = ctx->module->strings[intern_idx];
            c.v.string.length = (uint32_t)strlen(c.v.string.data);
        }
        maps.const_map[i] = bytecode_add_constant(ctx->module, c);
    }

    for (uint32_t i = 0; i < src->string_count; i++) {
        maps.string_map[i] = bytecode_add_string(ctx->module, src->strings[i], (uint32_t)strlen(src->strings[i]));
    }

    for (uint32_t i = 0; i < src->data_req_count; i++) {
        const DataRequirement *req = &src->data_reqs[i];
        const char *query = req->query ? req->query : "";
        uint32_t query_len = req->query ? req->query_len : 0u;
        uint16_t dst_idx = bytecode_add_data_req(
            ctx->module,
            req->name ? req->name : "",
            query,
            query_len,
            req->is_single,
            req->is_dynamic,
            req->line,
            req->column,
            req->param_count,
            (const char **)req->param_names,
            req->param_slots
        );
        /* Keep signature deterministic with original bytecode. */
        ctx->module->data_reqs[dst_idx].signature = req->signature;
        maps.data_req_map[i] = dst_idx;
    }

    for (uint32_t i = 0; i < src->dep_count; i++) {
        uint32_t old_ref = 0, old_func = 0;
        if (parse_linked_marker(src->deps[i].path, &old_ref, &old_func)) {
            /* Skip raw linked markers from nested modules; we re-emit them
             * remapped below via set_linked_target(). */
            maps.dep_map[i] = LINKED_NONE;
            continue;
        }
        maps.dep_map[i] = bytecode_add_dependency(ctx->module, src->deps[i].path);
    }

    for (uint32_t i = 0; i < src->builtin_count; i++) {
        maps.builtin_map[i] = bytecode_add_builtin(
            ctx->module,
            src->builtins[i].name,
            src->builtins[i].min_args,
            src->builtins[i].max_args
        );
    }

    for (uint32_t i = 0; i < src->comp_ref_count; i++) {
        maps.comp_ref_map[i] = bytecode_add_comp_ref(
            ctx->module,
            src->comp_refs[i].name,
            src->comp_refs[i].path
        );
        if (!ensure_linked_targets(ctx, ctx->module->comp_ref_count)) {
            free_merge_maps(&maps);
            return false;
        }
    }

    for (uint32_t i = 0; i < src->func_count; i++) {
        uint16_t dst_idx = bytecode_add_function(ctx->module);
        maps.func_map[i] = dst_idx;
        if (src->func_debug && i < src->func_debug_cap) {
            const FunctionDebugRef *fd = &src->func_debug[i];
            if ((fd->name && fd->name_len > 0) || (fd->source_path && fd->source_path_len > 0)) {
                bytecode_set_function_debug(ctx->module, dst_idx, fd->name, fd->source_path);
            }
        }
    }

    for (uint32_t i = 0; i < src->func_count; i++) {
        Chunk *dst_chunk = bytecode_get_function(ctx->module, maps.func_map[i]);
        if (!dst_chunk) {
            set_error(ctx, "failed to access destination function chunk during linking");
            free_merge_maps(&maps);
            return false;
        }
        if (!remap_chunk_code(ctx, src, &maps, &src->functions[i], dst_chunk)) {
            free_merge_maps(&maps);
            return false;
        }
    }

    for (uint32_t i = 0; i < src->dep_count; i++) {
        uint32_t old_ref = 0, old_func = 0;
        if (!parse_linked_marker(src->deps[i].path, &old_ref, &old_func)) continue;
        if (old_ref >= src->comp_ref_count || old_func >= src->func_count) continue;
        if (!set_linked_target(ctx, maps.comp_ref_map[old_ref], maps.func_map[old_func])) {
            free_merge_maps(&maps);
            return false;
        }
    }

    src_entry_idx = choose_component_entry_function(src, component_name, component_path);
    if (src_entry_idx == LINKED_NONE || src_entry_idx >= src->func_count) {
        set_error(ctx, "failed to determine linked component entry function");
        free_merge_maps(&maps);
        return false;
    }
    *out_entry_func_idx = maps.func_map[src_entry_idx];
    free_merge_maps(&maps);
    return true;
}

static bool cache_lookup_path(LinkerCtx *ctx, const char *path, uint16_t *out_func) {
    for (PathCacheEntry *it = ctx ? ctx->cache : NULL; it; it = it->next) {
        if (str_eq(it->path, path)) {
            if (out_func) *out_func = it->func_idx;
            return true;
        }
    }
    return false;
}

static bool cache_insert_path(LinkerCtx *ctx, const char *path, uint16_t func_idx) {
    PathCacheEntry *entry;
    if (!ctx || !path) return false;
    entry = (PathCacheEntry *)malloc(sizeof(PathCacheEntry));
    if (!entry) {
        set_error(ctx, "out of memory updating linker cache");
        return false;
    }
    entry->path = path;
    entry->func_idx = func_idx;
    entry->next = ctx->cache;
    ctx->cache = entry;
    return true;
}

static bool ensure_ref_resolved(LinkerCtx *ctx, uint16_t ref_idx) {
    const ComponentRef *ref;
    uint16_t cached_idx = LINKED_NONE;
    BytecodeModule *linked_mod;
    uint16_t entry_idx = LINKED_NONE;
    if (!ctx || !ctx->module) return false;
    if (!ensure_linked_targets(ctx, ctx->module->comp_ref_count)) return false;
    if (ref_idx >= ctx->module->comp_ref_count) {
        set_error(ctx, "linked ref index out of range");
        return false;
    }
    if (ctx->linked_targets[ref_idx] != LINKED_NONE) return true;

    ref = &ctx->module->comp_refs[ref_idx];
    if (!ref->path || !ref->path[0]) return true;

    if (cache_lookup_path(ctx, ref->path, &cached_idx) && cached_idx != LINKED_NONE) {
        return set_linked_target(ctx, ref_idx, cached_idx);
    }

    if (path_on_stack(ctx, ref->path)) {
        set_error(ctx, "cyclic linked component dependency detected");
        return false;
    }
    if (!push_path(ctx, ref->path)) return false;

    linked_mod = ctx->resolver ? ctx->resolver(ref->path, ctx->resolver_userdata) : NULL;
    if (!linked_mod) {
        char msg[256];
        (void)snprintf(msg, sizeof(msg),
                       "linked component module not found for path: %s",
                       ref->path ? ref->path : "<unknown>");
        set_error(ctx, msg);
        pop_path(ctx);
        return false;
    }

    if (!merge_component_module(ctx, linked_mod, ref->name, ref->path, &entry_idx)) {
        pop_path(ctx);
        return false;
    }
    if (entry_idx != LINKED_NONE) {
        if (!cache_insert_path(ctx, ref->path, entry_idx)) {
            pop_path(ctx);
            return false;
        }
        if (!set_linked_target(ctx, ref_idx, entry_idx)) {
            pop_path(ctx);
            return false;
        }
    }
    pop_path(ctx);
    return true;
}

static bool scan_chunk_for_linked_refs(LinkerCtx *ctx, const Chunk *chunk) {
    uint32_t ip = 0;
    if (!ctx || !chunk) return false;
    while (ip < chunk->code_len) {
        uint8_t op = chunk->code[ip];
        int operand_size = opcode_operand_size(op);
        uint32_t insn_size = 1u + (uint32_t)((operand_size < 0) ? 0 : operand_size);
        if (ip + insn_size > chunk->code_len) {
            set_error(ctx, "invalid opcode stream while scanning linked refs");
            return false;
        }
        if ((OpCode)op == BC_COMPONENT_LINKED) {
            uint16_t ref_idx = read_u16_le(chunk->code + ip + 1u);
            if (!ensure_ref_resolved(ctx, ref_idx)) return false;
        }
        ip += insn_size;
    }
    return true;
}

bool mot_link_resolve_module(
    BytecodeModule *module,
    MotLinkedModuleResolverFn resolver,
    void *resolver_userdata,
    char *error_buf,
    size_t error_buf_len
) {
    LinkerCtx ctx;
    uint32_t base_func_count;
    bool ok = true;
    if (!module || !resolver) return false;

    memset(&ctx, 0, sizeof(ctx));
    ctx.module = module;
    ctx.resolver = resolver;
    ctx.resolver_userdata = resolver_userdata;
    ctx.error_buf = error_buf;
    ctx.error_buf_len = error_buf_len;
    set_error(&ctx, NULL);

    if (!ensure_linked_targets(&ctx, module->comp_ref_count)) ok = false;
    if (ok) load_existing_markers(&ctx);

    if (ok) ok = scan_chunk_for_linked_refs(&ctx, &module->main);
    base_func_count = module->func_count;
    for (uint32_t i = 0; ok && i < base_func_count; i++) {
        ok = scan_chunk_for_linked_refs(&ctx, &module->functions[i]);
    }

    free(ctx.linked_targets);
    while (ctx.cache) {
        PathCacheEntry *next = ctx.cache->next;
        free(ctx.cache);
        ctx.cache = next;
    }
    while (ctx.stack) {
        PathStackEntry *next = ctx.stack->next;
        free(ctx.stack);
        ctx.stack = next;
    }
    return ok;
}
