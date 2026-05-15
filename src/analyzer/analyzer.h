/*
 * Semantic analyzer for Motus
 *
 * Performs:
 * - Scope resolution and symbol table building
 * - Type checking
 * - Dependency tracking
 * - Static vs dynamic classification
 */

#ifndef MOT_ANALYZER_H
#define MOT_ANALYZER_H

#include <stdbool.h>
#include "../util/arena.h"
#include "../parser/ast.h"
#include "../schema/schema_reader.h"
#include "scope.h"
#include "types.h"
#include "deps.h"

/* Analysis error */
typedef struct AnalysisError {
    const char *message;
    int line;
    int col;
    struct AnalysisError *next;
} AnalysisError;

/* Analysis result for an expression */
typedef struct {
    Type *type;
    DepSet *deps;
    bool is_static;      /* Can be evaluated at compile time */
    bool is_constant;    /* Is a literal constant */
} ExprResult;

/* Analyzer state */
typedef struct Analyzer {
    Arena *arena;
    TypeContext *types;
    DepContext *deps;
    Scope *scope;            /* Current scope */
    Scope *global_scope;     /* Module-level scope */

    /* Error tracking */
    AnalysisError *errors;
    AnalysisError **error_tail;
    int error_count;

    /* Schema registry */
    SchemaFile **schemas;    /* Array of imported schema files */
    uint32_t schema_count;
    uint32_t schema_cap;

    /* Analysis options */
    bool strict_types;       /* Require explicit types */
    bool track_deps;         /* Enable dependency tracking */
} Analyzer;

/* Create analyzer */
Analyzer *analyzer_new(Arena *arena);

/* Analyze a document (entry point) */
bool analyzer_analyze(Analyzer *a, AstNode *doc);

/* Get analysis errors */
AnalysisError *analyzer_errors(Analyzer *a);
int analyzer_error_count(Analyzer *a);

/* Check if analysis succeeded */
bool analyzer_ok(Analyzer *a);

/* Analyze individual node types (for testing) */
ExprResult analyzer_analyze_expr(Analyzer *a, AstNode *expr);
void analyzer_analyze_node(Analyzer *a, AstNode *node);

/* Register built-in functions */
void analyzer_register_builtins(Analyzer *a);

/* Scope management */
void analyzer_push_scope(Analyzer *a, ScopeKind kind);
void analyzer_pop_scope(Analyzer *a);

/* Error reporting */
void analyzer_error(Analyzer *a, int line, int col, const char *fmt, ...);

/* Type helpers */
Type *analyzer_resolve_type_name(Analyzer *a, const char *name);

/* Schema integration */
void analyzer_add_schema(Analyzer *a, SchemaFile *schema);
Type *analyzer_schema_to_type(Analyzer *a, SchemaStruct *s);
SchemaStruct *analyzer_find_schema_for_table(Analyzer *a, const char *table_name);

#endif /* MOT_ANALYZER_H */
