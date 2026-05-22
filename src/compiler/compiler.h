/*
 * Compiler for Motus
 *
 * Compiles analyzed AST to bytecode with partial evaluation.
 * Static values are resolved at compile time; dynamic values
 * become "holes" in the bytecode filled at runtime.
 */

#ifndef MOT_COMPILER_H
#define MOT_COMPILER_H

#include <stdbool.h>
#include "../util/arena.h"
#include "../parser/ast.h"
#include "../analyzer/analyzer.h"
#include "bytecode.h"
#include "partial_eval.h"

/* Compile-time value (for partial evaluation) */
typedef struct {
    bool is_static;          /* True if value known at compile time */
    Constant value;          /* Value if static */
} CompileValue;

/* Local variable in current scope */
typedef struct Local {
    const char *name;
    int depth;               /* Scope depth */
    uint16_t slot;           /* Stack slot */
    bool is_captured;        /* Captured by closure */
    bool is_var;             /* True if declared with <var> (reactive state) */
    struct Local *next;
} Local;

/* Compiler scope */
typedef struct CompilerScope {
    struct CompilerScope *enclosing;
    Local *locals;
    int local_count;
    int scope_depth;

    /* For loop state */
    int loop_start;          /* Start of loop (for continue) */
    int loop_end_jump;       /* Jump to patch for break */
} CompilerScope;

/* Component registry entry */
typedef struct ComponentEntry {
    const char *name;        /* Component name */
    uint16_t func_idx;       /* Function chunk index */
    AstNode *node;           /* Original defcomp node */
    struct ComponentEntry *next;
} ComponentEntry;

/* Macro registry entry */
typedef struct MacroEntry {
    const char *name;        /* Macro name */
    AstNode *node;           /* Original macro node */
    struct MacroEntry *next;
} MacroEntry;

/* Dynamic import entry (component loaded at edge) */
typedef struct DynamicImportEntry {
    const char *name;        /* Component name (e.g., "Header") */
    const char *path;        /* Source path (e.g., "components/Header") */
    uint16_t ref_idx;        /* Index in bytecode comp_refs */
    bool linked;             /* Known to be linked/embedded in edge runtime */
    struct DynamicImportEntry *next;
} DynamicImportEntry;

typedef bool (*CompilerLinkedComponentResolverFn)(const char *path, void *userdata);

/* Compiler state */
typedef struct Compiler {
    Arena *arena;
    BytecodeModule *module;
    Chunk *current_chunk;    /* Currently emitting to */

    /* Scopes */
    CompilerScope *scope;
    int scope_depth;

    /* Analyzer for type info */
    Analyzer *analyzer;

    /* Component registry */
    ComponentEntry *components;
    MacroEntry *macros;
    DynamicImportEntry *dynamic_imports;  /* Components loaded at edge */

    /* Current component context (for <children>) */
    uint16_t current_component_slot;  /* Slot index for children */
    bool in_component;                /* Are we compiling a component body? */
    bool in_macro;                    /* Are we compiling inside a macro expansion? */
    AstNode *macro_children;          /* Invocation children for <children> expansion */

    /* Error tracking */
    bool had_error;
    const char *error_msg;
    int error_line;
    int error_col;

    /* Current source location */
    int current_line;
    uint32_t reactive_node_seq;      /* Stable ids for attribute-reactive element markers */
    uint32_t reactive_expr_seq;      /* Stable ids for expression-reactive bindings */

    /* Partial evaluation mode */
    bool partial_eval;       /* Enable partial evaluation */
    PartialEval *pe;         /* Partial evaluator state */

    /* Dynamic component linker hints */
    CompilerLinkedComponentResolverFn linked_component_resolver;
    void *linked_component_userdata;
} Compiler;

/* Create compiler */
Compiler *compiler_new(Arena *arena, Analyzer *analyzer);

/* Compile a document to bytecode */
BytecodeModule *compiler_compile(Compiler *c, AstNode *doc);

/* Check if compilation succeeded */
bool compiler_ok(Compiler *c);
const char *compiler_error(Compiler *c);

/* Enable/disable partial evaluation */
void compiler_set_partial_eval(Compiler *c, bool enabled);
void compiler_set_linked_component_resolver(
    Compiler *c,
    CompilerLinkedComponentResolverFn resolver,
    void *userdata
);

#endif /* MOT_COMPILER_H */
