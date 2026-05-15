/*
 * Motus public API implementation
 */

#include "mot.h"

#include <stdlib.h>
#include <string.h>

#include "util/arena.h"
#include "parser/parser.h"
#include "analyzer/analyzer.h"
#include "compiler/compiler.h"
#include "compiler/bytecode.h"
#include "codegen/codegen.h"
#include "schema/schema_reader.h"

/* ---- MotModule: opaque handle owning compiled module + arena ---- */
struct MotModule {
    Arena *arena;
    BytecodeModule *module;
};

static char *heap_strdup(const char *s) {
    if (!s) return NULL;
    size_t len = strlen(s);
    char *copy = malloc(len + 1);
    if (!copy) return NULL;
    memcpy(copy, s, len + 1);
    return copy;
}

static void append_error(MotCompileResult *result, const char *message, int line, int column) {
    if (!result) return;

    if (result->errors.count >= result->errors.capacity) {
        size_t new_cap = result->errors.capacity == 0 ? 8 : result->errors.capacity * 2;
        MotError *new_errors = realloc(result->errors.errors, new_cap * sizeof(MotError));
        if (!new_errors) return;
        result->errors.errors = new_errors;
        result->errors.capacity = new_cap;
    }

    MotError *err = &result->errors.errors[result->errors.count++];
    err->message = heap_strdup(message ? message : "Unknown error");
    err->file = NULL;
    err->line = line;
    err->column = column;
}

static void append_parser_errors(MotCompileResult *result, MotErrorList *errors) {
    if (!errors) return;
    for (size_t i = 0; i < errors->count; i++) {
        MotError *err = &errors->errors[i];
        append_error(result, err->message, err->line, err->column);
    }
}

/* Append an error to a standalone MotErrorList (heap-allocated messages). */
static void append_error_to_list(MotErrorList *list, const char *message, int line, int column) {
    if (!list) return;

    if (list->count >= list->capacity) {
        size_t new_cap = list->capacity == 0 ? 8 : list->capacity * 2;
        MotError *new_errors = realloc(list->errors, new_cap * sizeof(MotError));
        if (!new_errors) return;
        list->errors = new_errors;
        list->capacity = new_cap;
    }

    MotError *err = &list->errors[list->count++];
    err->message = heap_strdup(message ? message : "Unknown error");
    err->file = NULL;
    err->line = line;
    err->column = column;
}

static MotCompileOptions resolve_options(const MotCompileOptions *options) {
    MotCompileOptions resolved;
    resolved.target = MOT_TARGET_BYTECODE;
    resolved.partial_eval = true;
    resolved.include_debug = true;
    resolved.linked_component_resolver = NULL;
    resolved.linked_component_userdata = NULL;
    resolved.schemas = NULL;
    resolved.schema_count = 0;

    if (!options) {
        return resolved;
    }
    resolved.target = options->target;
    resolved.partial_eval = options->partial_eval;
    resolved.include_debug = options->include_debug;
    resolved.linked_component_resolver = options->linked_component_resolver;
    resolved.linked_component_userdata = options->linked_component_userdata;
    resolved.schemas = options->schemas;
    resolved.schema_count = options->schema_count;
    if (resolved.target != MOT_TARGET_BYTECODE &&
        resolved.target != MOT_TARGET_WASM &&
        resolved.target != MOT_TARGET_BOTH) {
        resolved.target = MOT_TARGET_BYTECODE;
    }
    return resolved;
}

struct AstNode *mot_parse(const char *source, size_t source_len, struct Arena *arena, MotErrorList *errors) {
    if (!source || !arena) {
        if (errors && errors->count < errors->capacity) {
            MotError *err = &errors->errors[errors->count++];
            err->message = "Invalid input";
            err->file = NULL;
            err->line = 0;
            err->column = 0;
        }
        return NULL;
    }

    Parser parser;
    parser_init(&parser, source, source_len, arena, errors);
    return parser_parse(&parser);
}

/*
 * Internal: analyze + compile an AST, returning the BytecodeModule on success.
 * Errors are appended to *errors (if non-NULL).
 * On failure returns NULL (arena is NOT destroyed – caller decides).
 */
static BytecodeModule *compile_ast_internal(Arena *arena, AstNode *doc,
                                            const MotCompileOptions *opts,
                                            MotErrorList *errors) {
    Analyzer *analyzer = analyzer_new(arena);

    /* Register schemas before analysis */
    for (uint32_t si = 0; si < opts->schema_count; si++) {
        analyzer_add_schema(analyzer, opts->schemas[si]);
    }

    if (!analyzer_analyze(analyzer, doc)) {
        for (AnalysisError *err = analyzer_errors(analyzer); err; err = err->next) {
            append_error_to_list(errors, err->message, err->line, err->col);
        }
        if (!errors || errors->count == 0) {
            append_error_to_list(errors, "Analysis failed", 0, 0);
        }
        return NULL;
    }

    Compiler *compiler = compiler_new(arena, analyzer);
    compiler_set_partial_eval(compiler, opts->partial_eval);
    compiler_set_linked_component_resolver(
        compiler,
        opts->linked_component_resolver,
        opts->linked_component_userdata
    );
    BytecodeModule *module = compiler_compile(compiler, doc);
    if (!module || !compiler_ok(compiler)) {
        append_error_to_list(errors, compiler_error(compiler),
                             compiler->error_line, compiler->error_col);
        return NULL;
    }
    return module;
}

MotCompileResult mot_compile_with_options(const char *source, size_t source_len, const MotCompileOptions *options) {
    MotCompileResult result;
    MotCompileOptions resolved = resolve_options(options);
    memset(&result, 0, sizeof(result));

    if (!source) {
        append_error(&result, "Source is NULL", 0, 0);
        return result;
    }

    if (resolved.target == MOT_TARGET_WASM || resolved.target == MOT_TARGET_BOTH) {
        append_error(
            &result,
            "WASM target is not implemented in the core compiler yet; use runtime-wasm build pipeline",
            0,
            0
        );
        return result;
    }

    Arena *arena = arena_create(64 * 1024);
    if (!arena) {
        append_error(&result, "Failed to create arena", 0, 0);
        return result;
    }

    /* Parser requires preallocated error storage. */
    MotErrorList parse_errors;
    parse_errors.count = 0;
    parse_errors.capacity = 64;
    parse_errors.errors = arena_alloc(arena, parse_errors.capacity * sizeof(MotError));

    Parser parser;
    parser_init(&parser, source, source_len, arena, &parse_errors);
    AstNode *doc = parser_parse(&parser);
    if (!doc || parser_had_error(&parser)) {
        append_parser_errors(&result, &parse_errors);
        if (result.errors.count == 0) {
            append_error(&result, "Parse failed", 0, 0);
        }
        arena_destroy(arena);
        return result;
    }

    BytecodeModule *module = compile_ast_internal(arena, doc, &resolved, &result.errors);
    if (!module) {
        arena_destroy(arena);
        return result;
    }

    uint32_t bytecode_len = 0;
    uint8_t *bytecode = bytecode_serialize_ex(module, &bytecode_len, resolved.include_debug);
    if (bytecode_len > 0) {
        result.bytecode = malloc(bytecode_len);
        if (!result.bytecode) {
            append_error(&result, "Out of memory copying bytecode", 0, 0);
            arena_destroy(arena);
            return result;
        }
        memcpy(result.bytecode, bytecode, bytecode_len);
        result.bytecode_len = bytecode_len;
    }

    CodegenResult *assets = codegen_extract(arena, doc);
    char *css = codegen_combine_css(arena, assets);
    char *js = codegen_combine_js(arena, assets);

    result.css = heap_strdup(css ? css : "");
    result.js = heap_strdup(js ? js : "");
    if (!result.css || !result.js) {
        append_error(&result, "Out of memory copying assets", 0, 0);
    }

    arena_destroy(arena);
    return result;
}

MotCompileResult mot_compile(const char *source, size_t source_len) {
    MotCompileOptions options;
    options.target = MOT_TARGET_BYTECODE;
    options.partial_eval = true;
    options.include_debug = true;
    options.linked_component_resolver = NULL;
    options.linked_component_userdata = NULL;
    options.schemas = NULL;
    options.schema_count = 0;
    return mot_compile_with_options(source, source_len, &options);
}

void mot_result_free(MotCompileResult *result) {
    if (!result) return;

    free(result->bytecode);
    free(result->wasm);
    free(result->source_map_json);
    free(result->css);
    free(result->js);

    if (result->errors.errors) {
        for (size_t i = 0; i < result->errors.count; i++) {
            free((char *)result->errors.errors[i].message);
        }
    }
    free(result->errors.errors);

    memset(result, 0, sizeof(*result));
}

/* ---- Module handle API ---- */

MotModule *mot_compile_to_module(const char *source, size_t source_len,
                                  const MotCompileOptions *options,
                                  MotErrorList *errors) {
    if (!source) {
        append_error_to_list(errors, "Source is NULL", 0, 0);
        return NULL;
    }

    MotCompileOptions resolved = resolve_options(options);

    Arena *arena = arena_create(64 * 1024);
    if (!arena) {
        append_error_to_list(errors, "Failed to create arena", 0, 0);
        return NULL;
    }

    /* Parse */
    MotErrorList parse_errors;
    parse_errors.count = 0;
    parse_errors.capacity = 64;
    parse_errors.errors = arena_alloc(arena, parse_errors.capacity * sizeof(MotError));

    Parser parser;
    parser_init(&parser, source, source_len, arena, &parse_errors);
    AstNode *doc = parser_parse(&parser);
    if (!doc || parser_had_error(&parser)) {
        /* Copy arena-allocated parse errors to the caller's heap list. */
        for (size_t i = 0; i < parse_errors.count; i++) {
            MotError *e = &parse_errors.errors[i];
            append_error_to_list(errors, e->message, e->line, e->column);
        }
        if (!errors || errors->count == 0) {
            append_error_to_list(errors, "Parse failed", 0, 0);
        }
        arena_destroy(arena);
        return NULL;
    }

    return mot_compile_ast(arena, doc, &resolved, errors);
}

MotModule *mot_compile_ast(struct Arena *arena, struct AstNode *ast,
                            const MotCompileOptions *options,
                            MotErrorList *errors) {
    if (!arena || !ast) {
        append_error_to_list(errors, "Invalid input", 0, 0);
        return NULL;
    }

    MotCompileOptions resolved = resolve_options(options);

    BytecodeModule *module = compile_ast_internal(arena, ast, &resolved, errors);
    if (!module) {
        return NULL;  /* caller still owns arena */
    }

    /* Success – wrap in an opaque handle that owns the arena. */
    MotModule *mod = malloc(sizeof(MotModule));
    if (!mod) {
        append_error_to_list(errors, "Out of memory", 0, 0);
        return NULL;  /* caller still owns arena */
    }
    mod->arena = arena;
    mod->module = module;
    return mod;
}

BytecodeModule *mot_module_bytecode(MotModule *mod) {
    return mod ? mod->module : NULL;
}

uint8_t *mot_module_serialize(MotModule *mod, uint32_t *out_len, bool include_debug) {
    if (!mod || !mod->module) {
        if (out_len) *out_len = 0;
        return NULL;
    }

    uint32_t len = 0;
    uint8_t *arena_bytes = bytecode_serialize_ex(mod->module, &len, include_debug);
    if (!arena_bytes || len == 0) {
        if (out_len) *out_len = 0;
        return NULL;
    }

    /* Return a malloc'd copy so the module handle stays valid. */
    uint8_t *copy = malloc(len);
    if (!copy) {
        if (out_len) *out_len = 0;
        return NULL;
    }
    memcpy(copy, arena_bytes, len);
    if (out_len) *out_len = len;
    return copy;
}

void mot_module_free(MotModule *mod) {
    if (!mod) return;
    if (mod->arena) arena_destroy(mod->arena);
    free(mod);
}

void mot_error_list_free(MotErrorList *errors) {
    if (!errors) return;
    for (size_t i = 0; i < errors->count; i++) {
        free((char *)errors->errors[i].message);
    }
    free(errors->errors);
    errors->errors = NULL;
    errors->count = 0;
    errors->capacity = 0;
}
