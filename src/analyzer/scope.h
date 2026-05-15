/*
 * Scope and symbol table for Motus
 */

#ifndef MOT_SCOPE_H
#define MOT_SCOPE_H

#include <stdbool.h>
#include <stddef.h>
#include "../util/arena.h"

/* Forward declarations - use struct X* in declarations below */
struct Type;
struct AstNode;

/* Symbol kinds */
typedef enum {
    SYM_LET,         /* let binding - scoped to children */
    SYM_VAR,         /* var binding - reactive state */
    SYM_PARAM,       /* for loop item/index, component prop */
    SYM_COMPONENT,   /* component definition */
    SYM_MACRO,       /* macro definition */
    SYM_SLOT,        /* slot definition */
    SYM_IMPORT,      /* imported symbol */
    SYM_BUILTIN,     /* built-in function */
    SYM_SCHEMA,      /* schema type from .capnp */
} SymbolKind;

/* Symbol flags */
typedef enum {
    SYM_FLAG_NONE     = 0,
    SYM_FLAG_DYNAMIC  = 1 << 0,  /* dynamic data (fetched at runtime) */
    SYM_FLAG_SINGLE   = 1 << 1,  /* single value (not array) */
    SYM_FLAG_EXPORTED = 1 << 2,  /* exported from module */
    SYM_FLAG_USED     = 1 << 3,  /* referenced somewhere */
} SymbolFlags;

/* Symbol entry */
typedef struct Symbol {
    char *name;
    SymbolKind kind;
    SymbolFlags flags;
    struct Type *type;       /* Resolved type (nullable until analyzed) */
    struct AstNode *def_node; /* AST node where defined */
    int def_line;
    int def_col;
    struct Symbol *next;     /* Hash bucket chain */
} Symbol;

/* Scope kinds */
typedef enum {
    SCOPE_GLOBAL,    /* Document/module level */
    SCOPE_COMPONENT, /* Inside defcomp */
    SCOPE_MACRO,     /* Inside macro */
    SCOPE_LET,       /* Inside let children */
    SCOPE_FOR,       /* Inside for loop */
    SCOPE_IF,        /* Inside if/elsif/else branch */
    SCOPE_MATCH,     /* Inside match/case */
} ScopeKind;

/* Hash table bucket count (power of 2 for fast modulo) */
#define SCOPE_BUCKETS 32

/* Scope - a lexical scope with symbol table */
typedef struct Scope {
    ScopeKind kind;
    struct Scope *parent;    /* Enclosing scope */
    Symbol *symbols[SCOPE_BUCKETS];  /* Hash table */
    Arena *arena;            /* For allocations */
    int depth;               /* Nesting depth */

    /* Component/macro context */
    Symbol *component;       /* Current component (if in one) */
    Symbol *macro;           /* Current macro (if in one) */
} Scope;

/* Create a new scope */
Scope *scope_new(Arena *arena, ScopeKind kind, Scope *parent);

/* Define a symbol in the current scope */
Symbol *scope_define(Scope *scope, const char *name, SymbolKind kind,
                     struct AstNode *def_node, int line, int col);

/* Look up a symbol, searching parent scopes */
Symbol *scope_lookup(Scope *scope, const char *name);

/* Look up a symbol only in the current scope (no parent search) */
Symbol *scope_lookup_local(Scope *scope, const char *name);

/* Check if a name is defined in the current scope */
bool scope_is_defined_local(Scope *scope, const char *name);

/* Set symbol type */
void symbol_set_type(Symbol *sym, struct Type *type);

/* Set symbol flags */
void symbol_set_flags(Symbol *sym, SymbolFlags flags);

/* Get component scope (walks up to find enclosing component) */
Scope *scope_get_component(Scope *scope);

/* Debug: print scope contents */
void scope_dump(Scope *scope, int indent);

#endif /* MOT_SCOPE_H */
