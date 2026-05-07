/*
 * Scope and symbol table implementation
 */

#include "scope.h"
#include <stdio.h>
#include <string.h>

/* FNV-1a hash for symbol lookup */
static unsigned int hash_name(const char *name) {
    unsigned int hash = 2166136261u;
    while (*name) {
        hash ^= (unsigned char)*name++;
        hash *= 16777619u;
    }
    return hash;
}

Scope *scope_new(Arena *arena, ScopeKind kind, Scope *parent) {
    Scope *scope = arena_alloc(arena, sizeof(Scope));
    scope->kind = kind;
    scope->parent = parent;
    scope->arena = arena;
    scope->depth = parent ? parent->depth + 1 : 0;
    scope->component = parent ? parent->component : NULL;
    scope->macro = parent ? parent->macro : NULL;

    /* Initialize hash buckets */
    for (int i = 0; i < SCOPE_BUCKETS; i++) {
        scope->symbols[i] = NULL;
    }

    return scope;
}

Symbol *scope_define(Scope *scope, const char *name, SymbolKind kind,
                     struct AstNode *def_node, int line, int col) {
    Symbol *sym = arena_alloc(scope->arena, sizeof(Symbol));
    sym->name = arena_strdup(scope->arena, name);
    sym->kind = kind;
    sym->flags = SYM_FLAG_NONE;
    sym->type = NULL;
    sym->def_node = def_node;
    sym->def_line = line;
    sym->def_col = col;

    /* Insert into hash table */
    unsigned int bucket = hash_name(name) % SCOPE_BUCKETS;
    sym->next = scope->symbols[bucket];
    scope->symbols[bucket] = sym;

    /* Track component/macro in scope */
    if (kind == SYM_COMPONENT) {
        scope->component = sym;
    } else if (kind == SYM_MACRO) {
        scope->macro = sym;
    }

    return sym;
}

Symbol *scope_lookup(Scope *scope, const char *name) {
    while (scope) {
        Symbol *sym = scope_lookup_local(scope, name);
        if (sym) return sym;
        scope = scope->parent;
    }
    return NULL;
}

Symbol *scope_lookup_local(Scope *scope, const char *name) {
    unsigned int bucket = hash_name(name) % SCOPE_BUCKETS;
    Symbol *sym = scope->symbols[bucket];

    while (sym) {
        if (strcmp(sym->name, name) == 0) {
            return sym;
        }
        sym = sym->next;
    }

    return NULL;
}

bool scope_is_defined_local(Scope *scope, const char *name) {
    return scope_lookup_local(scope, name) != NULL;
}

void symbol_set_type(Symbol *sym, struct Type *type) {
    sym->type = type;
}

void symbol_set_flags(Symbol *sym, SymbolFlags flags) {
    sym->flags = (SymbolFlags)(sym->flags | flags);
}

Scope *scope_get_component(Scope *scope) {
    while (scope) {
        if (scope->kind == SCOPE_COMPONENT) {
            return scope;
        }
        scope = scope->parent;
    }
    return NULL;
}

static const char *symbol_kind_str(SymbolKind kind) {
    switch (kind) {
        case SYM_LET: return "let";
        case SYM_VAR: return "var";
        case SYM_PARAM: return "param";
        case SYM_COMPONENT: return "component";
        case SYM_MACRO: return "macro";
        case SYM_SLOT: return "slot";
        case SYM_IMPORT: return "import";
        case SYM_BUILTIN: return "builtin";
    }
    return "unknown";
}

static const char *scope_kind_str(ScopeKind kind) {
    switch (kind) {
        case SCOPE_GLOBAL: return "global";
        case SCOPE_COMPONENT: return "component";
        case SCOPE_MACRO: return "macro";
        case SCOPE_LET: return "let";
        case SCOPE_FOR: return "for";
        case SCOPE_IF: return "if";
        case SCOPE_MATCH: return "match";
    }
    return "unknown";
}

void scope_dump(Scope *scope, int indent) {
    for (int i = 0; i < indent; i++) printf("  ");
    printf("Scope[%s, depth=%d]\n", scope_kind_str(scope->kind), scope->depth);

    for (int i = 0; i < SCOPE_BUCKETS; i++) {
        Symbol *sym = scope->symbols[i];
        while (sym) {
            for (int j = 0; j < indent + 1; j++) printf("  ");
            printf("- %s : %s", sym->name, symbol_kind_str(sym->kind));
            if (sym->flags & SYM_FLAG_DYNAMIC) printf(" [dynamic]");
            if (sym->flags & SYM_FLAG_SINGLE) printf(" [single]");
            if (sym->flags & SYM_FLAG_EXPORTED) printf(" [exported]");
            printf(" @%d:%d\n", sym->def_line, sym->def_col);
            sym = sym->next;
        }
    }
}
