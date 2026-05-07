/*
 * Code Generation for Motus
 *
 * Extracts and processes:
 * - CSS from <style> blocks (with component scoping)
 * - JavaScript from <script> blocks
 */

#ifndef MOT_CODEGEN_H
#define MOT_CODEGEN_H

#include <stdbool.h>
#include <stddef.h>
#include "../util/arena.h"
#include "../parser/ast.h"

/* Extracted CSS block */
typedef struct CSSBlock {
    const char *content;     /* Scoped CSS content */
    size_t length;
    const char *component;   /* Component name (for scoping), NULL for global */
    struct CSSBlock *next;
} CSSBlock;

/* Extracted JS block */
typedef struct JSBlock {
    const char *content;     /* JS content */
    size_t length;
    const char *component;   /* Component name, NULL for global */
    struct JSBlock *next;
} JSBlock;

/* Codegen result */
typedef struct {
    CSSBlock *css;           /* Linked list of CSS blocks */
    JSBlock *js;             /* Linked list of JS blocks */
    int css_count;
    int js_count;
} CodegenResult;

/* Extract CSS and JS from an AST */
CodegenResult *codegen_extract(Arena *arena, AstNode *doc);

/* Combine all CSS blocks into a single string */
char *codegen_combine_css(Arena *arena, CodegenResult *result);

/* Combine all JS blocks into a single string */
char *codegen_combine_js(Arena *arena, CodegenResult *result);

/* Scope CSS selectors with a component prefix */
char *css_scope(Arena *arena, const char *css, size_t length, const char *component);

#endif /* MOT_CODEGEN_H */
