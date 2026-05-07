/*
 * Motus - XML-based templating language
 * Public API header
 */

#ifndef MOT_H
#define MOT_H

#include <stddef.h>
#include <stdint.h>
#include <stdbool.h>
#include "transpiler/bytecode_to_wasm.h"
#include "linker/bytecode_linker.h"

/* Forward declarations - these are opaque types for the public API */
struct Arena;
struct AstNode;
struct Lexer;
struct Parser;
struct Compiler;

/* Error handling */
#ifndef MOT_ERROR_TYPES_DEFINED
#define MOT_ERROR_TYPES_DEFINED
typedef struct {
    const char *message;
    const char *file;
    int line;
    int column;
} MotError;

typedef struct {
    MotError *errors;
    size_t count;
    size_t capacity;
} MotErrorList;
#endif

/* Compilation result */
typedef struct {
    uint8_t *bytecode;
    size_t bytecode_len;
    uint8_t *wasm;
    size_t wasm_len;
    char *source_map_json;
    char *css;
    char *js;
    MotErrorList errors;
} MotCompileResult;

/* Compile targets */
typedef enum {
    MOT_TARGET_BYTECODE = 0,
    MOT_TARGET_WASM = 1,
    MOT_TARGET_BOTH = 2,
} MotCompileTarget;

typedef bool (*MotLinkedComponentResolverFn)(const char *path, void *userdata);

typedef struct {
    MotCompileTarget target;
    bool partial_eval;
    bool include_debug;
    MotLinkedComponentResolverFn linked_component_resolver;
    void *linked_component_userdata;
} MotCompileOptions;

/* Main API */

/* Compile a Motus source file to bytecode + assets */
MotCompileResult mot_compile(const char *source, size_t source_len);

/* Compile with explicit options/target selection */
MotCompileResult mot_compile_with_options(const char *source, size_t source_len, const MotCompileOptions *options);

/* Free compilation result */
void mot_result_free(MotCompileResult *result);

/* Parse only (for tooling) */
struct AstNode *mot_parse(const char *source, size_t source_len, struct Arena *arena, MotErrorList *errors);

/* ---- Module handle API ---- */

/* Opaque handle owning a compiled module and its backing memory. */
typedef struct MotModule MotModule;

/*
 * Compile source directly to an in-memory module (no serialization).
 * On success the returned handle owns all associated memory.
 * Errors are appended to *errors when non-NULL (caller must call
 * mot_error_list_free when done).
 */
MotModule *mot_compile_to_module(const char *source, size_t source_len,
                                  const MotCompileOptions *options,
                                  MotErrorList *errors);

/*
 * Compile a pre-parsed AST to a module.
 *
 * On SUCCESS the returned handle takes ownership of `arena`; the caller
 * must NOT destroy it (mot_module_free will).
 *
 * On FAILURE returns NULL and the caller still owns `arena`.
 */
MotModule *mot_compile_ast(struct Arena *arena, struct AstNode *ast,
                            const MotCompileOptions *options,
                            MotErrorList *errors);

/* Access the underlying BytecodeModule (valid while the handle is alive). */
BytecodeModule *mot_module_bytecode(MotModule *mod);

/* Serialize module to malloc'd bytes.  Caller must free() the result. */
uint8_t *mot_module_serialize(MotModule *mod, uint32_t *out_len, bool include_debug);

/* Free module handle and all associated memory. */
void mot_module_free(MotModule *mod);

/* Free the contents of a MotErrorList populated by the module API. */
void mot_error_list_free(MotErrorList *errors);

/* Version info */
#define MOT_VERSION_MAJOR 0
#define MOT_VERSION_MINOR 1
#define MOT_VERSION_PATCH 0

#endif /* MOT_H */
